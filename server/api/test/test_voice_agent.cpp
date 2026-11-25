#include "AgentController.h"
#include "MeetingModule.h"
#include "ApiResponse.h"
#include "ServiceManager.h"
#include "ScryptPasswordHasher.h"
#include "SqliteAccountStore.h"
#include <gtest/gtest.h>
#include <future>

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
