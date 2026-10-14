#include "ThreadManager.h"

#include <algorithm>
#include <exception>
#include <stdexcept>
#include <utility>
#if defined(__APPLE__) || defined(__linux__)
#include <pthread.h>
#endif

namespace utils
{
namespace detail
{
struct ThreadControl
{
    std::mutex mutex;
    ThreadStatus status;
    // Serialized with Join so a stop callback never outlives its owner.
    std::mutex stop_mutex;
    std::function<void()> stop;
    bool stop_requested = false;

    bool RequestStop()
    {
        std::lock_guard<std::mutex> lock(stop_mutex);
        if (!stop) return false;
        if (stop_requested) return true;
        stop_requested = true;
        {
            std::lock_guard<std::mutex> state_lock(mutex);
            if (status.state == ThreadState::Starting || status.state == ThreadState::Running)
                status.state = ThreadState::Stopping;
        }
        try { stop(); }
        catch (...) { stop_requested = false; throw; }
        return true;
    }
};
}

ThreadManager& ThreadManager::Instance()
{
    static auto* manager = new ThreadManager;
    return *manager;
}

ManagedThread ThreadManager::Start(ThreadOptions options, std::function<void()> run, std::function<void()> stop)
{
    if (options.group.empty() || options.name.empty() || !run || !stop)
        throw std::invalid_argument("managed thread requires a group, name, run and stop callback");
    auto control = std::make_shared<detail::ThreadControl>();
    control->status.options = std::move(options);
    control->status.started_at = std::chrono::steady_clock::now();
    control->stop = std::move(stop);
    {
        std::lock_guard<std::mutex> lock(mutex_);
        control->status.id = next_id_++;
        threads_.emplace(control->status.id, control);
    }
    ManagedThread handle;
    try
    {
        handle.thread_ = std::thread([control, run = std::move(run)] {
#if defined(__APPLE__)
            pthread_setname_np(control->status.options.name.substr(0, 63).c_str());
#elif defined(__linux__)
            pthread_setname_np(pthread_self(), control->status.options.name.substr(0, 15).c_str());
#endif
            {
                std::lock_guard<std::mutex> lock(control->mutex);
                control->status.native_id = std::this_thread::get_id();
                if (control->status.state == ThreadState::Starting)
                    control->status.state = ThreadState::Running;
            }
            bool failed = false;
            try
            {
                run();
                std::lock_guard<std::mutex> lock(control->mutex);
                control->status.state = ThreadState::Exited;
            }
            catch (const std::exception& error)
            {
                failed = true;
                std::lock_guard<std::mutex> lock(control->mutex);
                control->status.state = ThreadState::Failed;
                control->status.error = error.what();
            }
            catch (...)
            {
                failed = true;
                std::lock_guard<std::mutex> lock(control->mutex);
                control->status.state = ThreadState::Failed;
                control->status.error = "unknown thread exception";
            }
            if (failed)
            {
                try { control->RequestStop(); }
                catch (...)
                {
                    std::lock_guard<std::mutex> lock(control->mutex);
                    control->status.error += "; stop callback failed";
                }
            }
        });
    }
    catch (...)
    {
        Remove(control->status.id);
        throw;
    }
    handle.control_ = std::move(control);
    return handle;
}

std::vector<ThreadStatus> ThreadManager::Snapshot() const
{
    std::vector<std::shared_ptr<detail::ThreadControl>> controls;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        for (const auto& entry : threads_) controls.push_back(entry.second);
    }
    std::vector<ThreadStatus> result;
    result.reserve(controls.size());
    for (const auto& control : controls)
    {
        std::lock_guard<std::mutex> lock(control->mutex);
        result.push_back(control->status);
    }
    std::sort(result.begin(), result.end(), [](const auto& a, const auto& b) { return a.id < b.id; });
    return result;
}

ManagedThread ThreadManager::TrackCurrent(ThreadOptions options)
{
    if (options.group.empty() || options.name.empty())
        throw std::invalid_argument("thread registration requires a group and name");
    auto control = std::make_shared<detail::ThreadControl>();
    control->status.options = std::move(options);
    control->status.started_at = std::chrono::steady_clock::now();
    control->status.state = ThreadState::Running;
    control->status.native_id = std::this_thread::get_id();
    {
        std::lock_guard<std::mutex> lock(mutex_);
        control->status.id = next_id_++;
        threads_.emplace(control->status.id, control);
    }
    ManagedThread handle;
    handle.control_ = std::move(control);
    return handle;
}

bool ThreadManager::RequestStop(uint64_t id)
{
    std::shared_ptr<detail::ThreadControl> control;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        const auto found = threads_.find(id);
        if (found == threads_.end()) return false;
        control = found->second;
    }
    return control->RequestStop();
}

void ThreadManager::Remove(uint64_t id)
{
    std::lock_guard<std::mutex> lock(mutex_);
    threads_.erase(id);
}

ManagedThread::~ManagedThread() { Reset(); }
ManagedThread::ManagedThread(ManagedThread&& other) noexcept
    : thread_(std::move(other.thread_)), control_(std::move(other.control_)) {}
ManagedThread& ManagedThread::operator=(ManagedThread&& other) noexcept
{
    if (this != &other)
    {
        Reset();
        thread_ = std::move(other.thread_);
        control_ = std::move(other.control_);
    }
    return *this;
}
uint64_t ManagedThread::Id() const noexcept { return control_ ? control_->status.id : 0; }
bool ManagedThread::Finished() const
{
    if (!control_) return true;
    std::lock_guard<std::mutex> lock(control_->mutex);
    return control_->status.state == ThreadState::Exited || control_->status.state == ThreadState::Failed;
}
void ManagedThread::RequestStop() { if (control_) control_->RequestStop(); }
void ManagedThread::Join()
{
    if (thread_.joinable())
    {
        if (GetId() == std::this_thread::get_id())
            throw std::logic_error("a managed thread cannot join itself; retain its handle on the owner");
        thread_.join();
    }
    if (control_)
    {
        {
            std::lock_guard<std::mutex> lock(control_->stop_mutex);
            control_->stop = {};
        }
        ThreadManager::Instance().Remove(control_->status.id);
        control_.reset();
    }
}
void ManagedThread::Reset() noexcept
{
    try { RequestStop(); Join(); }
    catch (...) { std::terminate(); }
}
}
