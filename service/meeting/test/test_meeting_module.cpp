#include "MeetingModule.h"
#include "ServiceManager.h"
#include <gtest/gtest.h>
#include <filesystem>
#include <future>
#include <thread>

namespace
{
using namespace service;
using namespace service::meeting;
using namespace std::chrono_literals;
using Json = nlohmann::json;
const std::string secret(40, 'x');
const RoomCommandSender sender = [](const auto&, const auto&) { return RoomCommandResult{200, {{"type", "ok"}}}; };

void ExpectUnavailable(const std::function<void()>& operation)
{
    try { operation(); FAIL() << "operation accepted outside Running"; }
    catch (const cluster::Failure& error) { EXPECT_EQ(error.status, 503); EXPECT_STREQ(error.code, "meeting_unavailable"); }
}
TEST(MeetingModule, AllPublicOperationsRequireRunningAndRestartPreservesState)
{
    auto module = std::make_shared<MeetingModule>(MeetingModuleOptions{":memory:", "test", secret}, sender);
    ServiceManager services;
    ASSERT_TRUE(services.Register(module));
    ExpectUnavailable([&] { module->CreateRoom("alice"); });
    ASSERT_TRUE(services.InitAll());
    ExpectUnavailable([&] { module->RegisterNode({"sfu-a", "boot-a", "ws://localhost:12345", 10, 0}); });
    ASSERT_TRUE(services.StartAll());
    ASSERT_EQ(services.Find(ServiceType::Meeting), module);
    EXPECT_TRUE(services.HealthSnapshot().front().healthy);
    module->RegisterNode({"sfu-a", "boot-a", "ws://localhost:12345", 10, 0});
    const auto room = module->CreateRoom("alice");
    module->SetMemberRole(room, "alice", "bob", false);
    const auto admission = module->Admit(room, "bob", "Bob");
    EXPECT_FALSE(admission.assignment.assignment_id.empty());
    services.StopAll();
    ExpectUnavailable([&] { module->CreateRoom("alice"); });
    ExpectUnavailable([&] { module->SetMemberRole(room, "alice", "bob", true); });
    ExpectUnavailable([&] { module->Admit(room, "bob", "Bob"); });
    ExpectUnavailable([&] { module->ManageRoom(room, "alice", {{"type", "participants"}}); });
    ExpectUnavailable([&] { module->EndRoom(room, "alice"); });
    ExpectUnavailable([&] { module->RegisterNode({"sfu-a", "boot-a", "ws://localhost:12345", 10, 0}); });
    ExpectUnavailable([&] { module->HeartbeatNode("sfu-a", "boot-a", 0); });
    ExpectUnavailable([&] { module->ReleaseRoom("sfu-a", "boot-a", room, admission.assignment.assignment_id); });
    ASSERT_TRUE(services.StartAll());
    EXPECT_EQ(module->Admit(room, "bob", "Bob").assignment.assignment_id, admission.assignment.assignment_id);
    EXPECT_EQ(module->ManageRoom(room, "alice", {{"type", "participants"}}).status, 200);
    services.ShutdownAll(); services.ShutdownAll();
    EXPECT_EQ(module->State(), ServiceState::Created);
    ExpectUnavailable([&] { module->CreateRoom("alice"); });
    ASSERT_TRUE(services.InitAll() && services.StartAll());
    EXPECT_NO_THROW(module->CreateRoom("alice"));
    services.ShutdownAll();
}
TEST(MeetingModule, StopRejectsNewWorkAndWaitsForAcceptedNetworkCommand)
{
    std::promise<void> entered, release;
    const auto gate = release.get_future().share();
    auto ready = entered.get_future();
    MeetingModule module({":memory:", "test", secret}, [&](const auto&, const auto&) {
        entered.set_value(); gate.wait_for(3s);
        return RoomCommandResult{200, {{"type", "ok"}}};
    });
    ASSERT_TRUE(module.Init() && module.Start());
    module.RegisterNode({"sfu-a", "boot-a", "ws://localhost:12345", 10, 0});
    const auto room = module.CreateRoom("alice");
    module.Admit(room, "alice", "Alice");
    auto command = std::async(std::launch::async, [&] { return module.ManageRoom(room, "alice", {{"type", "participants"}}); });
    ASSERT_EQ(ready.wait_for(2s), std::future_status::ready);
    auto stopped = std::async(std::launch::async, [&] { module.Stop(); });
    const auto deadline = std::chrono::steady_clock::now() + 1s;
    while (module.State() != ServiceState::Stopping && std::chrono::steady_clock::now() < deadline)
        std::this_thread::sleep_for(1ms);
    EXPECT_EQ(module.State(), ServiceState::Stopping);
    EXPECT_FALSE(module.Health().healthy);
    ExpectUnavailable([&] { module.CreateRoom("alice"); });
    EXPECT_EQ(stopped.wait_for(10ms), std::future_status::timeout);
    release.set_value();
    EXPECT_EQ(command.get().status, 200);
    ASSERT_EQ(stopped.wait_for(1s), std::future_status::ready);
    stopped.get();
    EXPECT_EQ(module.State(), ServiceState::Stopped);
    EXPECT_TRUE(module.Start());
    module.Shutdown();
}
TEST(MeetingModule, FailedInitializationReleasesResourcesAndCanBeRetriedAfterShutdown)
{
    const auto directory = std::filesystem::temp_directory_path() / ("meeting-module-" + cluster::Coordinator::RandomId());
    const auto path = directory / "cluster.db";
    {
        MeetingModule module({path.string(), "test", secret}, sender);
        EXPECT_FALSE(std::filesystem::exists(path));
        EXPECT_FALSE(module.Init());
        EXPECT_EQ(module.State(), ServiceState::Failed);
        EXPECT_FALSE(module.Health().healthy);
        EXPECT_NE(module.Health().error_code, 0);
        ExpectUnavailable([&] { module.CreateRoom("alice"); });
        module.Shutdown();
        std::filesystem::create_directory(directory);
        ASSERT_TRUE(module.Init() && module.Start());
        const auto room = module.CreateRoom("alice");
        module.Shutdown();
        ASSERT_TRUE(module.Init() && module.Start());
        // 未分配节点的会议结束不依赖网络；验证重新初始化读取的是同一个持久库。
        EXPECT_EQ(module.EndRoom(room, "alice").status, 200);
    }
    std::filesystem::remove_all(directory);
}
TEST(MeetingModule, InvalidOptionsFailWithoutOpeningDatabase)
{
    MeetingModule module({":memory:", "test", "short"}, sender);
    EXPECT_FALSE(module.Init());
    EXPECT_EQ(module.Health().error_code, 1);
    module.Shutdown();
    EXPECT_EQ(module.State(), ServiceState::Created);
    MeetingModule missing_sender({":memory:", "test", secret}, {});
    EXPECT_FALSE(missing_sender.Init());
}
}
