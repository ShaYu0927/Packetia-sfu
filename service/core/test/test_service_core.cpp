#include "ServiceBase.h"
#include "ServiceManager.h"
#include <gtest/gtest.h>
#include <future>
#include <stdexcept>
#include <string>
#include <vector>

namespace
{
class Probe final : public service::ServiceBase
{
public:
    Probe(service::ServiceType type, std::vector<std::string>& events) : type_(type), events_(events) {}
    ~Probe() override { Shutdown(); }
    service::ServiceType Type() const override { return type_; }
    bool fail_init = false, throw_start = false, fail_stop = false, initialized = false, running = false;
private:
    bool OnInit() override
    {
        events_.push_back("init"); initialized = true;
        return !fail_init;
    }
    bool OnStart() override
    {
        events_.push_back("start"); running = true;
        if (throw_start) throw std::runtime_error("start failed");
        return initialized;
    }
    void OnStop() override
    {
        events_.push_back("stop");
        if (fail_stop) { SetFailed(77); return; }
        running = false;
    }
    void OnShutdown() override { events_.push_back("shutdown"); initialized = false; }
    service::ServiceType type_;
    std::vector<std::string>& events_;
};
TEST(ServiceCore, LifecycleIsIdempotentAndReportsHealth)
{
    using namespace service;
    std::vector<std::string> events;
    Probe probe(ServiceType::Meeting, events);
    EXPECT_FALSE(probe.Start());
    EXPECT_FALSE(probe.Health().healthy);
    EXPECT_TRUE(probe.Init()); EXPECT_TRUE(probe.Init());
    EXPECT_EQ(events, (std::vector<std::string>{"init"}));
    EXPECT_TRUE(probe.Start()); EXPECT_TRUE(probe.Start());
    EXPECT_EQ(probe.State(), ServiceState::Running);
    EXPECT_TRUE(probe.Health().healthy);
    probe.Stop(); probe.Stop();
    EXPECT_EQ(probe.State(), ServiceState::Stopped);
    EXPECT_FALSE(probe.Health().healthy);
    EXPECT_TRUE(probe.Start());
    probe.Shutdown();
    EXPECT_EQ(probe.State(), ServiceState::Created);
    EXPECT_FALSE(probe.running); EXPECT_FALSE(probe.initialized);
    const auto count = events.size();
    probe.Shutdown();
    EXPECT_EQ(events.size(), count);
    EXPECT_TRUE(probe.Init() && probe.Start());
}
TEST(ServiceCore, InitializationFailureRollsBackCurrentAndEarlierServices)
{
    using namespace service;
    std::vector<std::string> first_events, second_events;
    auto first = std::make_shared<Probe>(ServiceType::Monitor, first_events);
    auto second = std::make_shared<Probe>(ServiceType::Meeting, second_events);
    second->fail_init = true;
    ServiceManager manager;
    EXPECT_FALSE(manager.Register(nullptr));
    ASSERT_TRUE(manager.Register(first)); ASSERT_TRUE(manager.Register(second));
    EXPECT_FALSE(manager.Register(second));
    EXPECT_EQ(manager.Find(ServiceType::Meeting), second);
    EXPECT_FALSE(manager.InitAll());
    EXPECT_FALSE(first->initialized); EXPECT_FALSE(second->initialized);
    EXPECT_EQ(first->State(), ServiceState::Created);
    EXPECT_EQ(second->State(), ServiceState::Failed);
    EXPECT_NE(second->Health().error_code, 0);
    EXPECT_EQ(first_events.back(), "shutdown");
    EXPECT_EQ(second_events.back(), "shutdown");
    manager.ShutdownAll();
    second->fail_init = false;
    ASSERT_TRUE(manager.InitAll() && manager.StartAll());
    EXPECT_TRUE(manager.HealthSnapshot().at(0).healthy);
    EXPECT_TRUE(manager.HealthSnapshot().at(1).healthy);
    manager.StopAll(); manager.ShutdownAll();
}
TEST(ServiceCore, StartExceptionRollsBackAndShutdownAllowsRetry)
{
    using namespace service;
    std::vector<std::string> first_events, second_events;
    auto first = std::make_shared<Probe>(ServiceType::Monitor, first_events);
    auto second = std::make_shared<Probe>(ServiceType::Meeting, second_events);
    second->throw_start = true;
    ServiceManager manager;
    ASSERT_TRUE(manager.Register(first) && manager.Register(second));
    ASSERT_TRUE(manager.InitAll());
    EXPECT_FALSE(manager.StartAll());
    EXPECT_FALSE(first->running); EXPECT_FALSE(second->running);
    EXPECT_EQ(first->State(), ServiceState::Stopped);
    EXPECT_EQ(second->State(), ServiceState::Failed);
    manager.ShutdownAll();
    second->throw_start = false;
    ASSERT_TRUE(manager.InitAll() && manager.StartAll());
    manager.ShutdownAll();
}
TEST(ServiceCore, ConcurrentInitializationRunsHookOnce)
{
    std::vector<std::string> events;
    Probe probe(service::ServiceType::Meeting, events);
    auto first = std::async(std::launch::async, [&] { return probe.Init(); });
    auto second = std::async(std::launch::async, [&] { return probe.Init(); });
    EXPECT_TRUE(first.get()); EXPECT_TRUE(second.get());
    EXPECT_EQ(events, (std::vector<std::string>{"init"}));
}
TEST(ServiceCore, StopFailureKeepsResourcesUntilCleanupCanSucceed)
{
    std::vector<std::string> events;
    Probe probe(service::ServiceType::Meeting, events);
    ASSERT_TRUE(probe.Init() && probe.Start());
    probe.fail_stop = true;
    probe.Stop();
    EXPECT_EQ(probe.State(), service::ServiceState::Failed);
    EXPECT_EQ(probe.Health().error_code, 77);
    probe.Shutdown();
    EXPECT_EQ(probe.State(), service::ServiceState::Failed);
    EXPECT_TRUE(probe.initialized);
    probe.fail_stop = false;
    probe.Shutdown();
    EXPECT_EQ(probe.State(), service::ServiceState::Created);
    EXPECT_FALSE(probe.initialized);
}
}
