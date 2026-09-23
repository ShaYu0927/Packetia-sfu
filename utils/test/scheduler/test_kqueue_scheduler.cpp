#include "EventLoop.h"
#include "KqueueTaskScheduler.h"

#include <chrono>
#include <cerrno>
#include <cstdlib>
#include <dlfcn.h>
#include <fcntl.h>
#include <future>
#include <iostream>
#include <stdexcept>
#include <system_error>
#include <sys/event.h>
#include <sys/socket.h>
#include <unistd.h>

namespace syscall_failure
{
thread_local bool next_write_registration = false;
}

// Exercise a partial kernel update without depending on descriptor races or
// changing the production scheduler's interface. All other calls reach libc.
extern "C" int kevent(int queue, const struct kevent* changes, int change_count,
    struct kevent* events, int event_count, const struct timespec* timeout)
{
    using Function = int (*)(int, const struct kevent*, int, struct kevent*, int,
        const struct timespec*);
    static const auto system_kevent = reinterpret_cast<Function>(::dlsym(RTLD_NEXT, "kevent"));
    if (!system_kevent) std::abort();
    if (syscall_failure::next_write_registration && change_count == 1
        && changes[0].filter == EVFILT_WRITE && (changes[0].flags & EV_ADD))
    {
        syscall_failure::next_write_registration = false;
        if ((changes[0].flags & EV_RECEIPT) && event_count > 0)
        {
            events[0] = changes[0];
            events[0].flags = EV_ERROR;
            events[0].data = ENOMEM;
            return 1;
        }
        errno = ENOMEM;
        return -1;
    }
    return system_kevent(queue, changes, change_count, events, event_count, timeout);
}

namespace
{
void Check(bool value, const char* expression, int line)
{
    if (!value) throw std::runtime_error(std::to_string(line) + ": " + expression);
}
#define CHECK(value) Check(static_cast<bool>(value), #value, __LINE__)

struct Descriptors
{
    int fd[2] = {-1, -1};
    ~Descriptors() { for (int value : fd) if (value >= 0) ::close(value); }
    void SocketPair()
    {
        CHECK(::socketpair(AF_UNIX, SOCK_STREAM, 0, fd) == 0);
        for (int value : fd)
            CHECK(::fcntl(value, F_SETFL, ::fcntl(value, F_GETFL) | O_NONBLOCK) == 0);
    }
    void Close(int index)
    {
        CHECK(::close(fd[index]) == 0);
        fd[index] = -1;
    }
};

void TasksTimersAndRestart()
{
    EventLoop loop(1);
    CHECK(loop.Start());
    auto scheduler = loop.GetTaskScheduler();
    CHECK(std::dynamic_pointer_cast<KqueueTaskScheduler>(scheduler));
    std::promise<bool> posted;
    auto posted_result = posted.get_future();
    CHECK(scheduler->Post([&] { posted.set_value(scheduler->IsCurrentThread()); }));
    CHECK(posted_result.wait_for(std::chrono::seconds(2)) == std::future_status::ready);
    CHECK(posted_result.get());
    bool invoked = false;
    scheduler->Invoke([&] { invoked = scheduler->IsCurrentThread(); });
    CHECK(invoked);

    std::promise<bool> timer;
    auto timer_result = timer.get_future();
    scheduler->AddTimer([&] {
        timer.set_value(scheduler->IsCurrentThread());
        return false;
    }, 1);
    CHECK(timer_result.wait_for(std::chrono::seconds(2)) == std::future_status::ready);
    CHECK(timer_result.get());
    const auto canceled = scheduler->AddTimer([] {
        throw std::runtime_error("canceled timer ran");
        return false;
    }, 10000);
    scheduler->RemoveTimer(canceled);

    loop.Stop();
    CHECK(!scheduler->Post([] {}));
    bool inline_cleanup = false;
    scheduler->Invoke([&] { inline_cleanup = !scheduler->IsCurrentThread(); });
    CHECK(inline_cleanup);
    CHECK(loop.Start());
    CHECK(loop.GetTaskScheduler() != scheduler);
    loop.Stop();
}

void ReadWriteAndHalfClose()
{
    KqueueTaskScheduler scheduler;
    Descriptors sockets;
    sockets.SocketPair();
    auto channel = std::make_shared<Channel>(sockets.fd[0]);
    int reads = 0;
    int writes = 0;
    int closes = 0;
    bool read_eof = false;
    channel->SetReadCallback([&] {
        char byte;
        const auto count = ::read(sockets.fd[0], &byte, 1);
        if (count == 0) read_eof = true;
        else { CHECK(count == 1); ++reads; }
    });
    channel->SetWriteCallback([&] { ++writes; });
    channel->SetCloseCallback([&] { ++closes; });
    channel->EnableReading();
    channel->EnableWriting();
    scheduler.UpdateChannel(channel);
    CHECK(::write(sockets.fd[1], "x", 1) == 1);
    CHECK(scheduler.HandleEvent(100));
    CHECK(reads == 1 && writes == 1 && closes == 0);

    CHECK(::shutdown(sockets.fd[1], SHUT_WR) == 0);
    CHECK(scheduler.HandleEvent(100));
    CHECK(read_eof && closes == 0);
    CHECK(::write(sockets.fd[0], "r", 1) == 1);
    char reply = 0;
    CHECK(::read(sockets.fd[1], &reply, 1) == 1 && reply == 'r');
    scheduler.RemoveChannel(channel);
}

void ReRegistrationSkipsOldWrite()
{
    KqueueTaskScheduler scheduler;
    Descriptors sockets;
    sockets.SocketPair();
    auto old_channel = std::make_shared<Channel>(sockets.fd[0]);
    auto new_channel = std::make_shared<Channel>(sockets.fd[0]);
    int old_writes = 0;
    int new_writes = 0;
    old_channel->SetWriteCallback([&] { ++old_writes; });
    new_channel->SetWriteCallback([&] {
        ++new_writes;
        scheduler.RemoveChannel(new_channel);
    });
    old_channel->SetReadCallback([&] {
        char byte;
        CHECK(::read(sockets.fd[0], &byte, 1) == 1);
        scheduler.RemoveChannel(old_channel);
        new_channel->EnableWriting();
        scheduler.UpdateChannel(new_channel);
    });
    old_channel->EnableReading();
    old_channel->EnableWriting();
    scheduler.UpdateChannel(old_channel);
    CHECK(::write(sockets.fd[1], "x", 1) == 1);
    CHECK(scheduler.HandleEvent(100));
    CHECK(old_writes == 0 && new_writes == 0);
    scheduler.RemoveChannel(old_channel); // Stale identity cannot remove the new channel.
    CHECK(scheduler.HandleEvent(100));
    CHECK(new_writes == 1);
}

void CallbackCanRemoveAnotherReadyChannel()
{
    KqueueTaskScheduler scheduler;
    Descriptors a;
    Descriptors b;
    a.SocketPair();
    b.SocketPair();
    auto first = std::make_shared<Channel>(a.fd[0]);
    auto second = std::make_shared<Channel>(b.fd[0]);
    int calls = 0;
    first->SetReadCallback([&] { ++calls; scheduler.RemoveChannel(second); });
    second->SetReadCallback([&] { ++calls; scheduler.RemoveChannel(first); });
    first->EnableReading();
    second->EnableReading();
    scheduler.UpdateChannel(first);
    scheduler.UpdateChannel(second);
    CHECK(::write(a.fd[1], "a", 1) == 1);
    CHECK(::write(b.fd[1], "b", 1) == 1);
    CHECK(scheduler.HandleEvent(100));
    CHECK(calls == 1);
    scheduler.RemoveChannel(first);
    scheduler.RemoveChannel(second);
}

void ReusedDescriptorKeepsNewIdentity()
{
    KqueueTaskScheduler scheduler;
    Descriptors old_sockets;
    old_sockets.SocketPair();
    const int reused_fd = old_sockets.fd[0];
    auto old_channel = std::make_shared<Channel>(reused_fd);
    int old_calls = 0;
    old_channel->SetReadCallback([&] { ++old_calls; });
    old_channel->EnableReading();
    scheduler.UpdateChannel(old_channel);
    old_sockets.Close(0); // The kernel automatically drops the old filters.

    Descriptors new_sockets;
    new_sockets.SocketPair();
    if (new_sockets.fd[0] != reused_fd)
    {
        CHECK(::dup2(new_sockets.fd[0], reused_fd) == reused_fd);
        new_sockets.Close(0);
        new_sockets.fd[0] = reused_fd;
    }
    auto new_channel = std::make_shared<Channel>(reused_fd);
    int new_calls = 0;
    new_channel->SetReadCallback([&] {
        char byte;
        CHECK(::read(reused_fd, &byte, 1) == 1);
        ++new_calls;
    });
    new_channel->EnableReading();
    new_channel->EnableWriting();
    syscall_failure::next_write_registration = true;
    bool failed = false;
    try { scheduler.UpdateChannel(new_channel); }
    catch (const std::system_error&) { failed = true; }
    CHECK(failed);
    CHECK(::write(new_sockets.fd[1], "n", 1) == 1);
    CHECK(scheduler.HandleEvent(0));
    CHECK(old_calls == 0 && new_calls == 0);
    new_channel->DisableWriting();
    scheduler.UpdateChannel(new_channel);
    scheduler.RemoveChannel(old_channel);
    CHECK(scheduler.HandleEvent(100));
    CHECK(old_calls == 0 && new_calls == 1);
    scheduler.RemoveChannel(new_channel);
}

void InvalidRegistrationDoesNotBreakScheduler()
{
    KqueueTaskScheduler scheduler;
    auto invalid = std::make_shared<Channel>(-1);
    invalid->EnableReading();
    bool failed = false;
    try { scheduler.UpdateChannel(invalid); }
    catch (const std::system_error&) { failed = true; }
    CHECK(failed);
    scheduler.RemoveChannel(invalid);
    Descriptors sockets;
    sockets.SocketPair();
    auto valid = std::make_shared<Channel>(sockets.fd[0]);
    int calls = 0;
    valid->SetReadCallback([&] { ++calls; });
    valid->EnableReading();
    scheduler.UpdateChannel(valid);
    CHECK(::write(sockets.fd[1], "v", 1) == 1);
    CHECK(scheduler.HandleEvent(100));
    CHECK(calls == 1);
    scheduler.RemoveChannel(valid);
}

void PartialRegistrationRestoresPreviousFilters()
{
    KqueueTaskScheduler scheduler;
    Descriptors sockets;
    sockets.SocketPair();
    auto channel = std::make_shared<Channel>(sockets.fd[0]);
    int reads = 0;
    int writes = 0;
    channel->SetReadCallback([&] {
        char byte;
        CHECK(::read(sockets.fd[0], &byte, 1) == 1);
        ++reads;
    });
    channel->SetWriteCallback([&] { ++writes; });
    channel->EnableReading();
    channel->EnableWriting();
    syscall_failure::next_write_registration = true;
    bool failed = false;
    try { scheduler.UpdateChannel(channel); }
    catch (const std::system_error& error) { failed = error.code().value() == ENOMEM; }
    CHECK(failed && !syscall_failure::next_write_registration);
    CHECK(::write(sockets.fd[1], "n", 1) == 1);
    CHECK(scheduler.HandleEvent(0));
    CHECK(reads == 0 && writes == 0); // Failed new registration published nothing.

    channel->DisableWriting();
    scheduler.UpdateChannel(channel);
    channel->EnableWriting();
    syscall_failure::next_write_registration = true;
    failed = false;
    try { scheduler.UpdateChannel(channel); }
    catch (const std::system_error& error) { failed = error.code().value() == ENOMEM; }
    CHECK(failed && !syscall_failure::next_write_registration);
    channel->DisableWriting();
    CHECK(scheduler.HandleEvent(100));
    CHECK(reads == 1 && writes == 0); // Old read filter and token were restored.
    scheduler.RemoveChannel(channel);
}

void WriteEofClosesOnce()
{
    KqueueTaskScheduler scheduler;
    Descriptors sockets;
    sockets.SocketPair();
    auto channel = std::make_shared<Channel>(sockets.fd[0]);
    int closes = 0;
    channel->SetCloseCallback([&] {
        ++closes;
        scheduler.RemoveChannel(channel);
    });
    channel->EnableWriting();
    scheduler.UpdateChannel(channel);
    sockets.Close(1);
    CHECK(scheduler.HandleEvent(100));
    CHECK(closes == 1);
    CHECK(scheduler.HandleEvent(0));
    CHECK(closes == 1);
}
} // namespace

int main()
{
    const std::pair<const char*, void (*)()> tests[] = {
        {"tasks, timers, stop and restart", TasksTimersAndRestart},
        {"simultaneous readiness and half-close", ReadWriteAndHalfClose},
        {"re-registration skips old write event", ReRegistrationSkipsOldWrite},
        {"callback removes another ready channel", CallbackCanRemoveAnotherReadyChannel},
        {"descriptor reuse preserves identity", ReusedDescriptorKeepsNewIdentity},
        {"registration error rolls back", InvalidRegistrationDoesNotBreakScheduler},
        {"partial registration restores previous filters", PartialRegistrationRestoresPreviousFilters},
        {"write EOF closes exactly once", WriteEofClosesOnce},
    };
    try
    {
        for (const auto& test : tests)
        {
            test.second();
            std::cout << "PASS: " << test.first << '\n';
        }
        return 0;
    }
    catch (const std::exception& error)
    {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
