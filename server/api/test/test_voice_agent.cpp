#include "AgentController.h"
#include "MeetingModule.h"
#include "ApiResponse.h"
#include "ServiceManager.h"
#include "ScryptPasswordHasher.h"
#include "SqliteAccountStore.h"
#include <gtest/gtest.h>
#include <future>
#include <algorithm>
#include <chrono>
#include <thread>

namespace
{
using Json = nlohmann::json;
using service::cluster::Failure;

template <typename Operation>
void ExpectFailure(Operation operation, uint16_t status, const char* code)
{
    try { operation(); FAIL() << "Expected rejection"; }
    catch (const Failure& error)
    {
        EXPECT_EQ(error.status, status);
        EXPECT_STREQ(error.code, code);
    }
}

class VoiceAgentTest : public ::testing::Test
{
protected:
    std::shared_ptr<service::meeting::MeetingModule> meetings;
    std::shared_ptr<service::ai::VoiceAgentService> agents;
    service::ServiceManager services;

    void SetUp() override
    {
        meetings = std::make_shared<service::meeting::MeetingModule>(
            service::meeting::MeetingModuleOptions{":memory:", "test", std::string(40, 'j')},
            [](const auto&, const auto&) {
                return service::meeting::RoomCommandResult{200, {{"type", "ok"}}};
            });
        agents = std::make_shared<service::ai::VoiceAgentService>(meetings, 2);
        ASSERT_TRUE(services.Register(meetings));
        ASSERT_TRUE(services.Register(agents));
        ASSERT_TRUE(services.InitAll());
        ASSERT_TRUE(services.StartAll());
    }
    void TearDown() override { services.StopAll(); services.ShutdownAll(); }
};

TEST_F(VoiceAgentTest, CapacityAndDuplicateRoomChecksReleaseSlotsOnStop)
{
    const auto room = meetings->CreateRoom("alice");
    const auto first = agents->CreateSession(room, "alice");
    EXPECT_EQ(first.state, service::ai::AgentSessionState::Prepared);
    ExpectFailure([&] { agents->CreateSession(room, "alice"); }, 409, "agent_already_exists");
    agents->CreateSession(meetings->CreateRoom("alice"), "alice");
    const auto third_room = meetings->CreateRoom("alice");
    ExpectFailure([&] { agents->CreateSession(third_room, "alice"); }, 409, "agent_capacity_reached");
    EXPECT_EQ(agents->StopSession(first.session_id, "alice").state, service::ai::AgentSessionState::Stopped);
    ExpectFailure([&] { agents->GetSession(first.session_id, "alice"); }, 404, "agent_not_found");
    ExpectFailure([&] { agents->StopSession(first.session_id, "alice"); }, 404, "agent_not_found");
    EXPECT_NO_THROW(agents->CreateSession(third_room, "alice"));
}

TEST_F(VoiceAgentTest, RequiresOpenRoomOwnerButAllowsCleanupAfterRoomEnds)
{
    const auto room = meetings->CreateRoom("alice");
    meetings->SetMemberRole(room, "alice", "bob", true);
    ExpectFailure([&] { agents->CreateSession(room, "bob"); }, 403, "room_owner_required");
    ExpectFailure([&] { agents->CreateSession("missing-room", "alice"); }, 403, "room_owner_required");
    const auto session = agents->CreateSession(room, "alice");
    ExpectFailure([&] { agents->GetSession(session.session_id, "bob"); }, 404, "agent_not_found");
    ExpectFailure([&] { agents->StopSession(session.session_id, "bob"); }, 404, "agent_not_found");
    EXPECT_EQ(agents->GetSession(session.session_id, "alice").room_id, room);
    meetings->EndRoom(room, "alice");
    ExpectFailure([&] { agents->CreateSession(room, "alice"); }, 410, "room_ended");
    EXPECT_NO_THROW(agents->StopSession(session.session_id, "alice"));
}

TEST_F(VoiceAgentTest, StopGatesCallsAndRestartDoesNotRestoreSessions)
{
    const auto room = meetings->CreateRoom("alice");
    const auto session = agents->CreateSession(room, "alice");
    services.StopAll();
    ExpectFailure([&] { agents->CreateSession(room, "alice"); }, 503, "agent_unavailable");
    ExpectFailure([&] { agents->GetSession(session.session_id, "alice"); }, 503, "agent_unavailable");
    ExpectFailure([&] { agents->StopSession(session.session_id, "alice"); }, 503, "agent_unavailable");
    ASSERT_TRUE(services.StartAll());
    ExpectFailure([&] { agents->GetSession(session.session_id, "alice"); }, 404, "agent_not_found");
    EXPECT_NO_THROW(agents->CreateSession(room, "alice"));
}

TEST_F(VoiceAgentTest, DisabledAgentRejectsCreationAfterOwnerCheck)
{
    service::ai::VoiceAgentService::Options options;
    options.enabled = false;
    service::ai::VoiceAgentService disabled(meetings, options);
    ASSERT_TRUE(disabled.Init());
    ASSERT_TRUE(disabled.Start());
    const auto room = meetings->CreateRoom("alice");
    ExpectFailure([&] { disabled.CreateSession(room, "bob"); }, 403, "room_owner_required");
    ExpectFailure([&] { disabled.CreateSession(room, "alice"); }, 503, "agent_disabled");
}

class TestAgentWorker final : public service::ai::AgentWorker
{
public:
    bool Start() override
    {
        running = true;
        return true;
    }

    void Stop() override
    {
        running = false;
        on_event = {};
    }

    bool StartSession(service::ai::AgentWorkerJob request, EventHandler handler) override
    {
        job = std::move(request);
        on_event = std::move(handler);
        return true;
    }

    void StopSession(const std::string& session_id) override { stopped_session = session_id; }

    bool running = false;
    service::ai::AgentWorkerJob job;
    EventHandler on_event;
    std::string stopped_session;
};

TEST_F(VoiceAgentTest, DrivesInjectedWorkerAndIgnoresLateJoinWhileStopping)
{
    using EventKind = service::ai::AgentWorkerEvent::Kind;
    using State = service::ai::AgentSessionState;
    auto worker = std::make_shared<TestAgentWorker>();
    service::ai::VoiceAgentService::Options options;
    options.worker = worker;
    service::ai::VoiceAgentService service(meetings, options);
    meetings->RegisterNode({"sfu-a", "boot-a", "ws://localhost:30000/webrtc", 10, 0});
    ASSERT_TRUE(service.Init());
    ASSERT_TRUE(service.Start());
    EXPECT_TRUE(worker->running);

    const auto room = meetings->CreateRoom("alice");
    const auto session = service.CreateSession(room, "alice");
    EXPECT_EQ(session.state, State::Starting);
    EXPECT_EQ(worker->job.session_id, session.session_id);
    EXPECT_EQ(worker->job.room_id, room);
    EXPECT_EQ(worker->job.agent_id, session.agent_id);
    EXPECT_FALSE(worker->job.join_token.empty());

    ASSERT_TRUE(worker->on_event);
    worker->on_event({EventKind::Joined, "participant-one", {}});
    const auto running = service.GetSession(session.session_id, "alice");
    EXPECT_EQ(running.state, State::Running);
    EXPECT_EQ(running.participant_id, "participant-one");

    EXPECT_EQ(service.StopSession(session.session_id, "alice").state, State::Stopping);
    EXPECT_EQ(worker->stopped_session, session.session_id);
    worker->on_event({EventKind::Joined, "participant-two", {}});
    EXPECT_EQ(service.GetSession(session.session_id, "alice").state, State::Stopping);
    worker->on_event({EventKind::Stopped, {}, {}});
    ExpectFailure([&] { service.GetSession(session.session_id, "alice"); }, 404, "agent_not_found");
    service.Stop();
    EXPECT_FALSE(worker->running);
}

TEST_F(VoiceAgentTest, ConcurrentCreationDoesNotCreateTwoAgentsForOneRoom)
{
    const auto room = meetings->CreateRoom("alice");
    const auto create = [&] {
        try { agents->CreateSession(room, "alice"); return 201; }
        catch (const Failure& error) { return int(error.status); }
    };
    auto first = std::async(std::launch::async, create);
    auto second = std::async(std::launch::async, create);
    const auto a = first.get(), b = second.get();
    EXPECT_TRUE((a == 201 && b == 409) || (a == 409 && b == 201));
}

TEST(VoiceAgentConfiguration, RejectsMissingDependenciesAndZeroCapacity)
{
    service::ai::VoiceAgentService missing(nullptr);
    EXPECT_FALSE(missing.Init());
    auto meetings = std::make_shared<service::meeting::MeetingModule>(
        service::meeting::MeetingModuleOptions{}, service::meeting::RoomCommandSender{});
    service::ai::VoiceAgentService zero(meetings, 0);
    EXPECT_FALSE(zero.Init());
    service::ai::VoiceAgentService unavailable(meetings);
    ASSERT_TRUE(unavailable.Init());
    EXPECT_FALSE(unavailable.Start());
}

// 即使断言提前返回，也先解开阻塞任务，再由 executor 析构回收线程。
struct TaskGate
{
    std::promise<void> promise;
    std::shared_future<void> ready = promise.get_future().share();
    bool released = false;
    void Release() { if (!released) { released = true; promise.set_value(); } }
    ~TaskGate() { Release(); }
};

TEST(AgentExecutorTest, DedicatedThreadBoundsQueueCancelsPendingAndCleansUpOnOwner)
{
    service::ai::AgentExecutor executor(1);
    TaskGate gate;
    std::promise<std::thread::id> cleanup;
    auto cleaned = cleanup.get_future();
    std::promise<std::thread::id> entered;
    auto running = entered.get_future();
    bool pending_executed = false;
    ASSERT_TRUE(executor.Start([&] { cleanup.set_value(std::this_thread::get_id()); }));
    // 清理回调不能引用比 executor 更早析构的局部对象。
    struct StopGuard { service::ai::AgentExecutor& executor; TaskGate& gate;
        ~StopGuard() { gate.Release(); executor.Stop(); } } stop{executor, gate};
    auto first = executor.Submit([&] {
        entered.set_value(std::this_thread::get_id());
        gate.ready.wait();
        return 7;
    });
    ASSERT_EQ(running.wait_for(std::chrono::seconds(2)), std::future_status::ready);
    const auto worker_id = running.get();
    EXPECT_NE(worker_id, std::this_thread::get_id());
    const auto threads = utils::ThreadManager::Instance().Snapshot();
    const auto worker = std::find_if(threads.begin(), threads.end(), [&](const auto& item) {
        return item.native_id == worker_id;
    });
    ASSERT_NE(worker, threads.end());
    EXPECT_EQ(worker->options.group, "agent");
    EXPECT_EQ(worker->options.model, utils::ThreadModel::Dedicated);

    auto pending = executor.Submit([&] { pending_executed = true; });
    ExpectFailure([&] { executor.Submit([] {}); }, 503, "agent_busy");
    ASSERT_TRUE(utils::ThreadManager::Instance().RequestStop(worker->id));
    ExpectFailure([&] { executor.Submit([] {}); }, 503, "agent_unavailable");
    gate.Release();
    EXPECT_EQ(first.get(), 7); // 已开始的任务允许完成。
    ExpectFailure([&] { pending.get(); }, 503, "agent_unavailable");
    executor.Stop();
    EXPECT_FALSE(pending_executed);
    EXPECT_EQ(cleaned.get(), worker_id);
    const auto remaining = utils::ThreadManager::Instance().Snapshot();
    EXPECT_TRUE(std::none_of(remaining.begin(), remaining.end(), [&](const auto& item) {
        return item.id == worker->id;
    }));
}

TEST(AgentExecutorTest, PropagatesErrorsSurvivesReentrancyAndRestarts)
{
    service::ai::AgentExecutor executor;
    ExpectFailure([&] { executor.Submit([] {}); }, 503, "agent_unavailable");
    ASSERT_TRUE(executor.Start({}));
    auto failed = executor.Submit([] { throw Failure(409, "test_failure"); });
    ExpectFailure([&] { failed.get(); }, 409, "test_failure");
    auto reentrant = executor.Submit([&] { executor.Submit([] {}).get(); });
    EXPECT_THROW(reentrant.get(), std::logic_error);
    EXPECT_EQ(executor.Submit([] { return 42; }).get(), 42);
    executor.Stop();
    executor.Stop();
    ASSERT_TRUE(executor.Start({}));
    EXPECT_EQ(executor.Submit([] { return 43; }).get(), 43);
    service::ai::AgentExecutor disabled(0);
    EXPECT_FALSE(disabled.Start({}));
}

TEST_F(VoiceAgentTest, ConcurrentRequestsAndStopCompleteWithoutLeakingOldSessions)
{
    const auto room = meetings->CreateRoom("alice");
    const auto session = agents->CreateSession(room, "alice");
    std::promise<void> start;
    auto ready = start.get_future().share();
    std::vector<std::future<int>> requests;
    for (int i = 0; i < 16; ++i)
        requests.push_back(std::async(std::launch::async, [&, ready] {
            ready.wait();
            try { agents->GetSession(session.session_id, "alice"); return 200; }
            catch (const Failure& error) { return int(error.status); }
        }));
    start.set_value();
    agents->Stop();
    for (auto& request : requests)
    {
        const auto status = request.get();
        EXPECT_TRUE(status == 200 || status == 503);
    }
    ASSERT_TRUE(agents->Start());
    ExpectFailure([&] { agents->GetSession(session.session_id, "alice"); }, 404, "agent_not_found");
    EXPECT_NO_THROW(agents->CreateSession(room, "alice"));
}

class AgentApiTest : public VoiceAgentTest
{
protected:
    std::shared_ptr<service::account::AccountService> accounts;
    std::unique_ptr<server::api::ApiRouter> routes;
    std::string token, owner, room;

    void SetUp() override
    {
        VoiceAgentTest::SetUp();
        service::account::AccountOptions options;
        options.enabled = options.registration_enabled = true;
        options.database_path = ":memory:";
        accounts = std::make_shared<service::account::AccountService>(options,
            std::make_shared<service::account::SqliteAccountStore>(),
            std::make_shared<service::account::ScryptPasswordHasher>());
        ASSERT_TRUE(accounts->Start().Succeeded());
        ASSERT_TRUE(accounts->CreateAccount({"alice", "correct-password-123", "Alice"}).Succeeded());
        const auto login = accounts->Login({"alice", "correct-password-123"});
        ASSERT_TRUE(login.Succeeded());
        token = login.value->session_token;
        owner = login.value->identity.user_id;
        room = meetings->CreateRoom(owner);
        server::api::ApiRouter::Builder builder;
        server::api::AgentController(accounts, agents).RegisterRoutes(builder);
        routes = std::make_unique<server::api::ApiRouter>(builder.Build());
    }
    void TearDown() override { VoiceAgentTest::TearDown(); if (accounts) accounts->Stop(); }
    protocol::http::HttpResponse Post(const std::string& path, const Json& json, const std::string& credential)
    {
        return routes->Dispatch({"POST", path, json.dump(),
            {{"content-type", "application/json"}, {"authorization", "Bearer " + credential}}});
    }
};

TEST_F(AgentApiTest, CreateQueryAndStopUseAuthenticatedIdentity)
{
    const auto created = Post("/api/v1/agent-sessions", {{"room_id", room}}, token);
    ASSERT_EQ(created.status, 201);
    const auto body = Json::parse(created.body);
    EXPECT_EQ(body.at("state"), "prepared");
    EXPECT_EQ(body.at("room_id"), room);
    EXPECT_FALSE(body.contains("owner_id"));
    const auto session = body.at("session_id");
    EXPECT_EQ(Post("/api/v1/agent-sessions/get", {{"session_id", session}}, token).status, 200);
    const auto stopped = Post("/api/v1/agent-sessions/stop", {{"session_id", session}}, token);
    ASSERT_EQ(stopped.status, 200);
    EXPECT_EQ(Json::parse(stopped.body).at("state"), "stopped");
    EXPECT_EQ(Post("/api/v1/agent-sessions/get", {{"session_id", session}}, token).status, 404);
}

TEST_F(AgentApiTest, RejectsInvalidBodiesForgedIdentityAndRevokedCredentials)
{
    EXPECT_EQ(Post("/api/v1/agent-sessions", {{"room_id", room}}, "").status, 401);
    EXPECT_EQ(Post("/api/v1/agent-sessions/get", {{"session_id", "unknown"}}, "").status, 401);
    EXPECT_EQ(Post("/api/v1/agent-sessions/stop", {{"session_id", "unknown"}}, "").status, 401);
    EXPECT_EQ(Post("/api/v1/agent-sessions", {{"room_id", room}, {"owner_id", owner}}, token).status, 400);
    EXPECT_EQ(Post("/api/v1/agent-sessions", {{"room_id", 1}}, token).status, 400);
    EXPECT_EQ(Post("/api/v1/agent-sessions", Json::object(), token).status, 400);
    EXPECT_EQ(Post("/api/v1/agent-sessions/get", {{"session_id", ""}}, token).status, 400);
    EXPECT_EQ(Post("/api/v1/agent-sessions/get", {{"session_id", std::string(129, 'a')}}, token).status, 400);
    ASSERT_TRUE(accounts->Logout(token).Succeeded());
    EXPECT_EQ(Post("/api/v1/agent-sessions", {{"room_id", room}}, token).status, 401);
}

TEST_F(AgentApiTest, OtherAccountCannotReadOrStopSessionAndStoppedServiceReturns503)
{
    const auto session = agents->CreateSession(room, owner);
    ASSERT_TRUE(accounts->CreateAccount({"bob", "correct-password-123", "Bob"}).Succeeded());
    const auto bob = accounts->Login({"bob", "correct-password-123"});
    ASSERT_TRUE(bob.Succeeded());
    EXPECT_EQ(Post("/api/v1/agent-sessions", {{"room_id", room}}, bob.value->session_token).status, 403);
    const Json request{{"session_id", session.session_id}};
    EXPECT_EQ(Post("/api/v1/agent-sessions/get", request, bob.value->session_token).status, 404);
    EXPECT_EQ(Post("/api/v1/agent-sessions/stop", request, bob.value->session_token).status, 404);
    EXPECT_EQ(Post("/api/v1/agent-sessions/get", request, token).status, 200);
    services.StopAll();
    EXPECT_EQ(Post("/api/v1/agent-sessions", {{"room_id", room}}, token).status, 503);
    EXPECT_EQ(Post("/api/v1/agent-sessions/get", request, token).status, 503);
    EXPECT_EQ(Post("/api/v1/agent-sessions/stop", request, token).status, 503);
}
}
