#pragma once

#include <chrono>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

namespace utils
{
enum class ThreadModel { Main, Io, Worker, Dedicated, Logging };
enum class ThreadState { Starting, Running, Stopping, Exited, Failed };

struct ThreadOptions
{
    std::string group;
    std::string name;
    ThreadModel model = ThreadModel::Worker;
};

struct ThreadStatus
{
    uint64_t id = 0;
    ThreadOptions options;
    ThreadState state = ThreadState::Starting;
    std::thread::id native_id;
    std::chrono::steady_clock::time_point started_at;
    std::string error;
};

namespace detail { struct ThreadControl; }
class ThreadManager;

class ManagedThread final
{
public:
    ManagedThread() = default;
    ~ManagedThread();
    ManagedThread(ManagedThread&& other) noexcept;
    ManagedThread& operator=(ManagedThread&& other) noexcept;
    ManagedThread(const ManagedThread&) = delete;
    ManagedThread& operator=(const ManagedThread&) = delete;

    void RequestStop();
    void Join();
    bool Joinable() const noexcept { return thread_.joinable(); }
    std::thread::id GetId() const noexcept { return thread_.get_id(); }
    uint64_t Id() const noexcept;
    bool Finished() const;
    std::thread::native_handle_type NativeHandle() { return thread_.native_handle(); }

private:
    friend class ThreadManager;
    void Reset() noexcept;
    std::thread thread_;
    std::shared_ptr<detail::ThreadControl> control_;
};


class ThreadManager final
{
public:
    static ThreadManager& Instance();
    ManagedThread Start(ThreadOptions options, std::function<void()> run, std::function<void()> stop);
    // Register a caller-owned thread for inspection; its handle only unregisters.
    ManagedThread TrackCurrent(ThreadOptions options);
    std::vector<ThreadStatus> Snapshot() const;
    bool RequestStop(uint64_t id);

private:
    friend class ManagedThread;
    ThreadManager() = default;
    ThreadManager(const ThreadManager&) = delete;
    ThreadManager& operator=(const ThreadManager&) = delete;
    void Remove(uint64_t id);
    mutable std::mutex mutex_;
    uint64_t next_id_ = 1;
    std::unordered_map<uint64_t, std::shared_ptr<detail::ThreadControl>> threads_;
};
}
