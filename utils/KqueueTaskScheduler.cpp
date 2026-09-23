#include "KqueueTaskScheduler.h"

#include <array>
#include <cerrno>
#include <fcntl.h>
#include <stdexcept>
#include <system_error>
#include <sys/event.h>
#include <unistd.h>

KqueueTaskScheduler::KqueueTaskScheduler(int id) : TaskScheduler(id)
{
    kqueuefd_ = ::kqueue();
    if (kqueuefd_ < 0)
        throw std::system_error(errno, std::generic_category(), "kqueue");

    try
    {
        if (::fcntl(kqueuefd_, F_SETFD, FD_CLOEXEC) < 0)
            throw std::system_error(errno, std::generic_category(), "kqueue close-on-exec");
        if (!wakeup_channel_)
            throw std::runtime_error("kqueue wakeup pipe was not created");
        UpdateChannel(wakeup_channel_);
    }
    catch (...)
    {
        ::close(kqueuefd_);
        kqueuefd_ = -1;
        throw;
    }
}

KqueueTaskScheduler::~KqueueTaskScheduler()
{
    if (kqueuefd_ >= 0) ::close(kqueuefd_);
}

int KqueueTaskScheduler::ChangeFilter(int fd, short filter, bool enabled,
    std::uintptr_t token)
{
    struct kevent change{};
    struct kevent receipt{};
    EV_SET(&change, fd, filter,
        (enabled ? EV_ADD | EV_ENABLE : EV_DELETE) | EV_RECEIPT,
        0, 0, reinterpret_cast<void*>(token));
    const timespec immediate{};
    int result;
    do
    {
        result = ::kevent(kqueuefd_, &change, 1, &receipt, 1, &immediate);
    } while (result < 0 && errno == EINTR);

    const int error = result < 0 ? errno
        : result == 1 && (receipt.flags & EV_ERROR) ? static_cast<int>(receipt.data)
        : EIO;
    // Closing a descriptor automatically deletes its filters from kqueue.
    if (!enabled && (error == ENOENT || error == EBADF)) return 0;
    return error;
}

int KqueueTaskScheduler::SetFilters(int fd, int events, std::uintptr_t token)
{
    int error = ChangeFilter(fd, EVFILT_READ, events & EVENT_IN, token);
    if (error != 0) return error;
    return ChangeFilter(fd, EVFILT_WRITE, events & EVENT_OUT, token);
}

void KqueueTaskScheduler::SetRegistration(std::shared_ptr<Channel> channel,
    int events, bool removing)
{
    if (!channel) return;
    const int fd = channel->GetSocket();

    // Retain replaced callbacks until after releasing the writer lock.
    Registration previous;
    std::lock_guard<std::mutex> lock(channel_mutex_);
    auto current = channels_.find(fd);
    if (current != channels_.end()) previous = current->second;
    // A delayed removal of an old channel must not remove a reused descriptor.
    if (removing && previous.channel != channel) return;
    if (!previous.channel && events == EVENT_NONE) return;

    if (events != EVENT_NONE && next_token_ == 0)
        throw std::overflow_error("kqueue registration token exhausted");
    const auto token = events == EVENT_NONE ? 0 : next_token_++;
    if (current == channels_.end())
        current = channels_.emplace(fd, Registration{}).first;

    const int error = SetFilters(fd, events, token);
    if (error != 0)
    {
        // Changes to read/write filters are separate operations. Restore the
        // previous registration if either fails. A different Channel may own
        // a reused fd, so never resurrect the old channel on that descriptor.
        const bool can_restore = previous.channel == channel;
        const int rollback_error = SetFilters(fd,
            can_restore ? previous.events : EVENT_NONE,
            can_restore ? previous.token : 0);
        if (!can_restore || rollback_error != 0)
        {
            SetFilters(fd, EVENT_NONE, 0);
            channels_.erase(current);
        }
        throw std::system_error(error, std::generic_category(), "kevent registration");
    }

    if (events == EVENT_NONE) channels_.erase(current);
    else current->second = {std::move(channel), token, events};
}

void KqueueTaskScheduler::UpdateChannel(std::shared_ptr<Channel> channel)
{
    if (!channel) return;
    const int requested = channel->GetEvents();
    const int events = (requested & (EVENT_IN | EVENT_PRI) ? EVENT_IN : 0)
        | (requested & EVENT_OUT);
    SetRegistration(std::move(channel), events, events == EVENT_NONE);
}

void KqueueTaskScheduler::RemoveChannel(std::shared_ptr<Channel>& channel)
{
    SetRegistration(channel, EVENT_NONE, true);
}

bool KqueueTaskScheduler::HandleEvent(int timeout)
{
    std::array<struct kevent, 512> events{};
    timespec wait{};
    if (timeout >= 0)
    {
        wait.tv_sec = timeout / 1000;
        wait.tv_nsec = (timeout % 1000) * 1000000;
    }
    const int count = ::kevent(kqueuefd_, nullptr, 0, events.data(),
        static_cast<int>(events.size()), timeout < 0 ? nullptr : &wait);
    if (count < 0) return errno == EINTR;

    struct Ready
    {
        int fd = -1;
        std::uintptr_t token = 0;
        int events = 0;
    };
    std::array<Ready, 512> ready{};
    std::size_t ready_count = 0;
    for (int i = 0; i < count; ++i)
    {
        const auto& event = events[i];
        const auto token = reinterpret_cast<std::uintptr_t>(event.udata);
        int mask = EVENT_NONE;
        if (event.flags & EV_ERROR) mask = EVENT_ERROR;
        else
        {
            if (event.filter == EVFILT_READ) mask |= EVENT_IN;
            if (event.filter == EVFILT_WRITE) mask |= EVENT_OUT;
            if (event.flags & EV_EOF)
            {
                if (event.fflags) mask |= EVENT_ERROR;
                // Read EOF can be a peer's write half-close. Let recv()==0
                // drive connection teardown after queued responses flush.
                else if (event.filter == EVFILT_READ) mask |= EVENT_RDHUP;
                else mask |= EVENT_HUP;
            }
        }

        // kqueue reports read and write filters separately. Coalesce them so
        // EOF/error is delivered once, after any readable data is consumed.
        std::size_t index = 0;
        while (index < ready_count && ready[index].token != token) ++index;
        if (index == ready_count)
            ready[ready_count++] = {static_cast<int>(event.ident), token, mask};
        else ready[index].events |= mask;
    }

    for (std::size_t i = 0; i < ready_count; ++i)
    {
        auto& item = ready[i];
        if (item.events & EVENT_ERROR) item.events &= ~EVENT_HUP;
        for (int kind : {EVENT_IN, EVENT_OUT, EVENT_HUP, EVENT_ERROR})
        {
            if (!(item.events & kind)) continue;
            std::shared_ptr<Channel> channel;
            {
                std::lock_guard<std::mutex> lock(channel_mutex_);
                const auto found = channels_.find(item.fd);
                if (found == channels_.end() || found->second.token != item.token) break;
                channel = found->second.channel;
            }
            // Recheck identity between callbacks: a read callback may remove
            // or re-register this descriptor before a queued write/EOF event.
            channel->HandleEvent(kind);
        }
    }
    return true;
}
