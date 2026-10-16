#include "ThreadManager.h"
#include "logger.h"

#include <atomic>
#include <condition_variable>
#include <filesystem>
#include <iostream>
#include <mutex>
#include <stdexcept>

#define CHECK(condition) do { if (!(condition)) throw std::runtime_error(#condition); } while (false)

namespace
{
using namespace utils;
using namespace std::chrono_literals;

struct Gate
{
    std::mutex mutex;
    std::condition_variable changed;
    bool entered = false;
    bool released = false;
    std::atomic<int> stops{0};
    std::atomic<int> completions{0};

    void Run()
    {
        std::unique_lock<std::mutex> lock(mutex);
        entered = true;
        changed.notify_all();
        changed.wait(lock, [&] { return released; });
        ++completions;
    }
    void Stop()
    {
        { std::lock_guard<std::mutex> lock(mutex); released = true; ++stops; }
        changed.notify_all();
    }
    void WaitEntered()
    {
        std::unique_lock<std::mutex> lock(mutex);
        CHECK(changed.wait_for(lock, 3s, [&] { return entered; }));
    }
};

ThreadStatus Find(uint64_t id)
{
    for (const auto& status : ThreadManager::Instance().Snapshot())
        if (status.id == id) return status;
    throw std::runtime_error("thread is missing from the registry");
}

void ModelsAndCooperativeStop()
{
    for (auto model : {ThreadModel::Io, ThreadModel::Worker, ThreadModel::Dedicated, ThreadModel::Logging})
    {
        Gate gate;
        auto thread = ThreadManager::Instance().Start({"models", "model-test", model},
            [&] { gate.Run(); }, [&] { gate.Stop(); });
        gate.WaitEntered();
        const auto id = thread.Id();
        const auto status = Find(id);
        CHECK(status.state == ThreadState::Running && status.options.model == model);
        CHECK(status.native_id == thread.GetId());
        CHECK(ThreadManager::Instance().RequestStop(id));
        thread.RequestStop();
        thread.Join();
        CHECK(gate.stops == 1 && gate.completions == 1);
        CHECK(!ThreadManager::Instance().RequestStop(id));
        thread.Join();
    }
}

void MoveAndDestructor()
{
    Gate first, second;
    uint64_t first_id = 0, second_id = 0;
    {
        auto a = ThreadManager::Instance().Start({"move", "first", ThreadModel::Worker},
            [&] { first.Run(); }, [&] { first.Stop(); });
        first.WaitEntered();
        first_id = a.Id();
        auto b = ThreadManager::Instance().Start({"move", "second", ThreadModel::Worker},
            [&] { second.Run(); }, [&] { second.Stop(); });
        second.WaitEntered();
        second_id = b.Id();
        b = std::move(a);
        CHECK(!a.Joinable() && a.Id() == 0 && b.Id() == first_id);
        CHECK(second.stops == 1 && second.completions == 1);
        CHECK(!ThreadManager::Instance().RequestStop(second_id));
    }
    CHECK(first.stops == 1 && first.completions == 1);
    CHECK(!ThreadManager::Instance().RequestStop(first_id));
}

void FailureAndSelfJoin()
{
    std::atomic<int> failure_stops{0};
    auto failed = ThreadManager::Instance().Start({"failure", "throw", ThreadModel::Dedicated},
        [] { throw std::runtime_error("test thread failure"); }, [&] { ++failure_stops; });
    const auto deadline = std::chrono::steady_clock::now() + 3s;
    while (!failed.Finished()) { CHECK(std::chrono::steady_clock::now() < deadline); std::this_thread::yield(); }
    CHECK(Find(failed.Id()).state == ThreadState::Failed);
    CHECK(Find(failed.Id()).error == "test thread failure");
    failed.Join();
    CHECK(failure_stops == 1);

    Gate gate;
    std::atomic<bool> rejected{false};
    ManagedThread thread;
    thread = ThreadManager::Instance().Start({"self", "self-join", ThreadModel::Dedicated},
        [&] {
            gate.Run();
            try { thread.Join(); } catch (const std::logic_error&) { rejected = true; }
        }, [&] { gate.Stop(); });
    gate.WaitEntered();
    thread.RequestStop();
    while (!thread.Finished()) { CHECK(std::chrono::steady_clock::now() < deadline); std::this_thread::yield(); }
    CHECK(rejected);
    thread.Join();
}

void ConcurrentInspectAndStop()
{
    Gate gate;
    auto target = ThreadManager::Instance().Start({"race", "target", ThreadModel::Dedicated},
        [&] { gate.Run(); }, [&] { gate.Stop(); });
    gate.WaitEntered();
    const auto id = target.Id();
    std::vector<std::thread> callers;
    for (int i = 0; i != 8; ++i)
        callers.emplace_back([id] {
            for (int j = 0; j != 100; ++j)
            {
                ThreadManager::Instance().Snapshot();
                ThreadManager::Instance().RequestStop(id);
            }
        });
    target.Join();
    for (auto& caller : callers) caller.join();
    CHECK(gate.stops == 1 && gate.completions == 1);
}

void CurrentAndLogger()
{
    const auto before = ThreadManager::Instance().Snapshot().size();
    {
        auto current = ThreadManager::Instance().TrackCurrent({"main", "test-main", ThreadModel::Main});
        CHECK(!current.Joinable());
        CHECK(Find(current.Id()).native_id == std::this_thread::get_id());
        CHECK(!ThreadManager::Instance().RequestStop(current.Id()));
    }
    CHECK(ThreadManager::Instance().Snapshot().size() == before);
    const auto path = std::filesystem::temp_directory_path() / "packetia-thread-logger-test.log";
    {
        AsyncFileLogger logger(path.string(), false);
        logger.Write(__FILE__, __LINE__, BaseLogger::INFO, "drained before join");
        const auto statuses = ThreadManager::Instance().Snapshot();
        CHECK(statuses.size() == before + 1 && statuses.back().options.model == ThreadModel::Logging);
    }
    CHECK(ThreadManager::Instance().Snapshot().size() == before);
    std::ifstream input(path);
    std::string line;
    std::getline(input, line);
    CHECK(line.find("drained before join") != std::string::npos);
    input.close();
    std::filesystem::remove(path);
}
}

int main()
try
{
    ModelsAndCooperativeStop();
    MoveAndDestructor();
    FailureAndSelfJoin();
    ConcurrentInspectAndStop();
    CurrentAndLogger();
    CHECK(utils::ThreadManager::Instance().Snapshot().empty());
    std::cout << "Thread manager lifecycle, all models, concurrent stop and logger tests passed\n";
    return 0;
}
catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
