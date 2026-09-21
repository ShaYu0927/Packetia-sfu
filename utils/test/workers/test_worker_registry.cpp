#include "WorkerRegistry.h"
#include "EndpointBase.h"
#include "server/ServerLauncher.h"
#include "server/WorkerSetup.h"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <iostream>
#include <mutex>
#include <stdexcept>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#define CHECK(expr) do { if (!(expr)) throw std::runtime_error( \
    std::string(__func__) + ":" + std::to_string(__LINE__) + ": " #expr); } while (false)

namespace {
using namespace std::chrono_literals;

WorkerPoolConfig Config(std::string name, std::size_t workers = 1)
{
    return {std::move(name), workers, 64, ShardedWorkerPool::DropPolicy::DropTail};
}

struct StopEvents
{
    std::mutex mutex;
    std::vector<std::string> names;
};

class Handler final : public IJobHandler
{
public:
    explicit Handler(std::string name = {}, std::shared_ptr<StopEvents> events = {})
        : name_(std::move(name)), events_(std::move(events)) {}

    void handle(WorkJob&) override { ++handled; }

    void on_worker_stop(std::size_t) override
    {
        ++stops;
        if (events_) {
            std::lock_guard<std::mutex> lock(events_->mutex);
            events_->names.push_back(name_);
        }
    }

    std::atomic<int> handled{0};
    std::atomic<int> stops{0};

private:
    std::string name_;
    std::shared_ptr<StopEvents> events_;
};

void PostJobs(const std::string& name, int count)
{
    for (int i = 0; i < count; ++i) {
        WorkJob job;
        job.type = WorkType::Control;
        job.key = 7;
        CHECK(WorkerService::post(name, std::move(job)) == 0);
    }
}

template<class Predicate>
void WaitUntil(Predicate ready)
{
    const auto deadline = std::chrono::steady_clock::now() + 3s;
    while (!ready()) {
        CHECK(std::chrono::steady_clock::now() < deadline);
        std::this_thread::sleep_for(1ms);
    }
}

// These guards release the worker and join the stopping thread even when an
// assertion fails, so a failed lifecycle test reports its cause instead of hanging.
class Gate
{
public:
    ~Gate() { Open(); }

    void Block()
    {
        std::unique_lock<std::mutex> lock(mutex_);
        entered_ = true;
        changed_.notify_all();
        changed_.wait(lock, [&] { return open_; });
    }

    void WaitEntered()
    {
        std::unique_lock<std::mutex> lock(mutex_);
        CHECK(changed_.wait_for(lock, 3s, [&] { return entered_; }));
    }

    void Open()
    {
        std::lock_guard<std::mutex> lock(mutex_);
        open_ = true;
        changed_.notify_all();
    }

private:
    std::mutex mutex_;
    std::condition_variable changed_;
    bool entered_ = false;
    bool open_ = false;
};

class StoppingThread
{
public:
    StoppingThread(WorkerRegistry& registry, Gate& gate, bool drain)
        : gate_(gate), thread_([&registry, drain] { registry.Stop(drain); }) {}

    ~StoppingThread() { Join(); }

    void Join()
    {
        gate_.Open();
        if (thread_.joinable()) thread_.join();
    }

private:
    Gate& gate_;
    std::thread thread_;
};

struct OpenGateOnExit
{
    std::shared_ptr<Gate> gate;
    ~OpenGateOnExit() { gate->Open(); }
};

void TestStopQueuedJobs(bool drain)
{
    auto handler = std::make_shared<Handler>();
    WorkerRegistry registry;
    auto gate = std::make_shared<Gate>();
    OpenGateOnExit release{gate};
    const std::string name = drain ? "registry-drain" : "registry-discard";
    registry.Add(Config(name), handler);
    CHECK(registry.Start() == 0);
    auto pool = WorkerService::get_pool_shared(name);
    CHECK(pool);
    CHECK(WorkerService::post_fn(name, [gate] { gate->Block(); }) == 0);
    gate->WaitEntered();
    PostJobs(name, 4);
    CHECK(pool->Status().queue_depth == 4);

    StoppingThread stopping(registry, *gate, drain);
    WaitUntil([&] { return !WorkerService::exists(name); });
    if (!drain) {
        // The in-flight function keeps the worker occupied until shutdown has
        // discarded all four queued jobs; no scheduler timing is assumed.
        WaitUntil([&] { return pool->Status().queue_depth == 0; });
    }
    stopping.Join();
    CHECK(handler->handled == (drain ? 4 : 0));
    CHECK(handler->stops == 1);
    registry.Stop(drain);
    CHECK(handler->stops == 1);
    CHECK(WorkerService::post_fn(name, [] {}) != 0);
}

void TestReverseOrderAndRunningRegistration()
{
    auto events = std::make_shared<StopEvents>();
    auto first = std::make_shared<Handler>("first", events);
    auto second = std::make_shared<Handler>("second", events);
    WorkerRegistry registry;
    registry.Add(Config("registry-first", 2), first);
    registry.Add(Config("registry-second"), second);
    CHECK(registry.Start() == 0);
    auto original = WorkerService::get_pool_shared("registry-first");
    CHECK(original && original->Status().worker_count == 2);
    CHECK(registry.Start() == 0);
    CHECK(WorkerService::get_pool_shared("registry-first") == original);

    bool rejected = false;
    try { registry.Add(Config("registry-late"), first); }
    catch (const std::logic_error&) { rejected = true; }
    CHECK(rejected);
    rejected = false;
    try { registry.AddFunction(Config("registry-late-function")); }
    catch (const std::logic_error&) { rejected = true; }
    CHECK(rejected);

    PostJobs("registry-first", 8);
    PostJobs("registry-second", 8);
    registry.Stop();
    CHECK(first->handled == 8 && second->handled == 8);
    CHECK(first->stops == 2 && second->stops == 1);
    CHECK((events->names == std::vector<std::string>{"second", "first", "first"}));
    CHECK(!WorkerService::exists("registry-late"));
    CHECK(!WorkerService::exists("registry-late-function"));
    registry.Stop();
    CHECK(events->names.size() == 3);
}

void TestFailedStartRollsBack(bool function_failure)
{
    auto handler = std::make_shared<Handler>();
    WorkerRegistry registry;
    registry.Add(Config("registry-before-failure"), handler);
    if (function_failure) registry.AddFunction(Config("registry-invalid", 0));
    else registry.Add(Config("registry-invalid", 0), std::make_shared<Handler>());
    registry.AddFunction(Config("registry-after-failure"));
    CHECK(registry.Start() != 0);
    CHECK(!WorkerService::exists("registry-before-failure"));
    CHECK(!WorkerService::exists("registry-invalid"));
    CHECK(!WorkerService::exists("registry-after-failure"));
    CHECK(handler->stops == 1);
    registry.Stop();
    CHECK(handler->stops == 1);
    CHECK(registry.Start() != 0);
    CHECK(handler->stops == 2);
}

void TestDuplicateNames(const std::string& first, const std::string& duplicate)
{
    WorkerRegistry registry;
    registry.AddFunction(Config(first));
    registry.Add(Config(duplicate), std::make_shared<Handler>());
    CHECK(registry.Start() != 0);
    CHECK(!WorkerService::exists(first));
    CHECK(!WorkerService::exists(duplicate));
    registry.Stop();
    CHECK(registry.Start() != 0);
    CHECK(!WorkerService::exists(first));
}

void TestExistingPoolSurvives(const std::string& existing, const std::string& requested)
{
    auto external = std::make_shared<Handler>();
    CHECK(WorkerService::create_pool(existing, 1, external) == 0);
    auto original = WorkerService::get_pool_shared(existing);
    auto owned = std::make_shared<Handler>();
    {
        WorkerRegistry registry;
        registry.Add(Config("registry-owned-before-conflict"), owned);
        registry.AddFunction(Config(requested));
        CHECK(registry.Start() != 0);
        CHECK(!WorkerService::exists("registry-owned-before-conflict"));
        CHECK(owned->stops == 1);
        CHECK(WorkerService::get_pool_shared(existing) == original);
        registry.Stop(false);
    }
    CHECK(WorkerService::get_pool_shared(requested) == original);
    CHECK(external->stops == 0);
    PostJobs(existing, 3);
    WorkerService::destroy_pool(existing, true);
    CHECK(external->handled == 3 && external->stops == 1);
}

void TestFunctionsRestartAndDestructor()
{
    auto handler = std::make_shared<Handler>();
    std::atomic<int> ran{0};
    {
        WorkerRegistry registry;
        registry.Stop();
        registry.Add(Config("registry-restart-typed"), handler);
        registry.AddFunction(Config("registry-functions", 2));
        CHECK(registry.Start() == 0);
        auto original = WorkerService::get_pool_shared("registry-functions");
        CHECK(original && original->Status().worker_count == 2);
        CHECK(registry.Start() == 0);
        CHECK(WorkerService::get_pool_shared("registry-functions") == original);
        for (int i = 0; i < 16; ++i) {
            CHECK(WorkerService::post_fn("registry-functions", i, [&] { ++ran; }) == 0);
        }
        PostJobs("registry-restart-typed", 2);
        registry.Stop();
        CHECK(ran == 16 && handler->handled == 2 && handler->stops == 1);
        CHECK(!WorkerService::exists("registry-functions"));
        CHECK(registry.Start() == 0);
        CHECK(WorkerService::get_pool_shared("registry-functions") != original);
        for (int i = 0; i < 16; ++i) {
            CHECK(WorkerService::post_fn("registry-functions", [&] { ++ran; }) == 0);
        }
        PostJobs("registry-restart-typed", 2);
        // Leaving the registry's scope must drain and stop the restarted pools.
    }
    CHECK(ran == 32 && handler->handled == 4 && handler->stops == 2);
    CHECK(!WorkerService::exists("registry-functions"));
    CHECK(!WorkerService::exists("registry-restart-typed"));
}

void TestEmptyRegistry()
{
    WorkerRegistry registry;
    registry.Stop();
    CHECK(registry.Start() == 0);
    CHECK(registry.Start() == 0);
    registry.Stop();
    registry.Stop(false);
    CHECK(registry.Start() == 0);
}

class TestEndpoint final : public utils::EndpointBase
{
public:
    TestEndpoint() : EndpointBase(utils::EndpointManager::Instance().AllocId(),
                                  "worker-setup-test") {}

    bool Start() override { SetState(State::kRunning); return true; }
    void Stop() override { SetState(State::kStopped); }

    std::atomic<int> rtp{0};
    std::atomic<int> rtcp{0};

protected:
    void OnRtp(WorkJob&) override { ++rtp; }
    void OnRtcp(WorkJob&) override { ++rtcp; }
};

void TestServerWorkerSetup()
{
    auto endpoint = std::make_shared<TestEndpoint>();
    auto& endpoints = utils::EndpointManager::Instance();
    CHECK(endpoint->Start());
    CHECK(endpoints.Add(endpoint));
    std::atomic<int> functions{0};
    auto post = [&](int count) {
        for (int i = 0; i < count; ++i) {
            WorkJob media;
            media.type = WorkType::Rtp;
            media.target_id = endpoint->Id();
            media.key = endpoint->Id() + 1000;
            CHECK(WorkerService::post(POOL_MEDIA, std::move(media)) == 0);
            WorkJob control;
            control.type = WorkType::Rtcp;
            control.target_id = endpoint->Id();
            control.key = endpoint->Id() + 2000;
            CHECK(WorkerService::post(POOL_ENDPOINT_ALIAS, std::move(control)) == 0);
            CHECK(WorkerService::post_fn(POOL_TRANSCODE, [&] { ++functions; }) == 0);
        }
    };
    {
        server::ServerLauncher launcher;
        server::AddWorkerPools(launcher);
        CHECK(launcher.StartAll());
        CHECK(launcher.StartAll());
        post(10);
        launcher.StopAll();
        CHECK(endpoint->rtp == 10 && endpoint->rtcp == 10 && functions == 10);
        CHECK(WorkerService::pool_names().empty());
        launcher.StopAll();
        CHECK(launcher.StartAll());
        post(3);
        // The launcher owns the registry throughout callback destruction.
    }
    CHECK(endpoint->rtp == 13 && endpoint->rtcp == 13 && functions == 13);
    CHECK(WorkerService::pool_names().empty());
    endpoint->Stop();
    CHECK(endpoints.Remove(endpoint->Id()));
}
} // namespace

int main()
try
{
    TestStopQueuedJobs(true);
    TestStopQueuedJobs(false);
    TestReverseOrderAndRunningRegistration();
    TestFailedStartRollsBack(false);
    TestFailedStartRollsBack(true);
    TestDuplicateNames("registry-duplicate", "registry-duplicate");
    TestDuplicateNames("Registry-Case", "registry-case");
    TestDuplicateNames(POOL_ENDPOINT, POOL_ENDPOINT_ALIAS);
    TestExistingPoolSurvives("registry-external", "REGISTRY-EXTERNAL");
    TestExistingPoolSurvives(POOL_ENDPOINT, POOL_ENDPOINT_ALIAS);
    TestFunctionsRestartAndDestructor();
    TestEmptyRegistry();
    TestServerWorkerSetup();
    CHECK(WorkerService::pool_names().empty());
    std::cout << "Worker registry lifecycle tests passed\n";
    return 0;
}
catch (const std::exception& error)
{
    WorkerService::destroy_all(false);
    std::cerr << error.what() << '\n';
    return 1;
}
