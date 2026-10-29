#include "NodeController.h"
#include "RoomController.h"
#include "MeetingModule.h"
#include "ServiceManager.h"
#include "ApiServer.h"
#include "ApiResponse.h"
#include "ScryptPasswordHasher.h"
#include "SqliteAccountStore.h"
#include "JwtAuthenticator.h"
#include "KeyProvider.h"
#include <gtest/gtest.h>
#include <future>

namespace
{
using Json = nlohmann::json;
using namespace server::api;
class ClusterApiTest : public ::testing::Test
{
protected:
    std::shared_ptr<service::account::AccountService> accounts;
    std::shared_ptr<service::meeting::MeetingModule> meetings;
    service::ServiceManager services;
    std::unique_ptr<ApiRouter> public_routes, internal_routes;
    std::string session, user;
    Json sent_command;
    service::cluster::RoomAssignment sent_route;
    uint16_t command_status = 200;
    const std::string node_secret = std::string(40, 'n'), jwt_secret = std::string(40, 'j');
    void SetUp() override
    {
        service::account::AccountOptions options;
        options.enabled = options.registration_enabled = true;
        options.database_path = ":memory:";
        accounts = std::make_shared<service::account::AccountService>(options,
            std::make_shared<service::account::SqliteAccountStore>(), std::make_shared<service::account::ScryptPasswordHasher>());
        ASSERT_TRUE(accounts->Start().Succeeded());
        auto registered = accounts->CreateAccount({"alice", "correct-password-123", "Alice"});
        ASSERT_TRUE(registered.Succeeded()); user = registered.value->id;
        auto login = accounts->Login({"alice", "correct-password-123"});
        ASSERT_TRUE(login.Succeeded()); session = login.value->session_token;
        meetings = std::make_shared<service::meeting::MeetingModule>(
            service::meeting::MeetingModuleOptions{":memory:", "test", jwt_secret}, [this](const auto& route, const auto& command) {
                sent_route = route; sent_command = command;
                return service::meeting::RoomCommandResult{command_status, {{"type", command_status == 200 ? "ok" : "error"}}};
            });
        ASSERT_TRUE(services.Register(meetings));
        ASSERT_TRUE(services.InitAll() && services.StartAll());
        server::meeting::RoomController rooms(accounts, meetings);
        server::cluster::NodeController nodes(meetings, {{"sfu-a", node_secret}});
        ApiRouter::Builder public_builder, internal_builder;
        rooms.RegisterRoutes(public_builder); nodes.RegisterRoutes(internal_builder);
        public_routes = std::make_unique<ApiRouter>(public_builder.Build());
        internal_routes = std::make_unique<ApiRouter>(internal_builder.Build());
    }
    void TearDown() override { services.StopAll(); services.ShutdownAll(); accounts->Stop(); }
    protocol::http::HttpResponse Post(const ApiRouter& router, std::string path, const Json& body, const std::string& token = "")
    {
        return router.Dispatch({"POST", std::move(path), body.dump(),
            {{"content-type", "application/json"}, {"authorization", "Bearer " + token}}});
    }
};
TEST_F(ClusterApiTest, PublicAndInternalRoutesHaveSeparateAuthentication)
{
    const Json registration = {{"node_id", "sfu-a"}, {"instance_id", "boot-a"},
        {"signaling_url", "ws://127.0.0.1:30000/webrtc"}, {"max_rooms", 100}, {"participants", 0}};
    EXPECT_EQ(Post(*public_routes, "/internal/v1/nodes/register", registration, node_secret).status, 404);
    EXPECT_EQ(Post(*internal_routes, "/api/v1/rooms", Json::object(), session).status, 404);
    EXPECT_EQ(Post(*internal_routes, "/internal/v1/nodes/register", registration, session).status, 401);
    EXPECT_EQ(Post(*internal_routes, "/internal/v1/nodes/register", registration, "").status, 401);
    EXPECT_EQ(Post(*internal_routes, "/internal/v1/nodes/register", registration, node_secret).status, 200);
    auto wrong_id = registration; wrong_id["node_id"] = "sfu-b";
    EXPECT_EQ(Post(*internal_routes, "/internal/v1/nodes/register", wrong_id, node_secret).status, 401);
    auto wrong_type = registration; wrong_type["max_rooms"] = "100";
    EXPECT_EQ(Post(*internal_routes, "/internal/v1/nodes/register", wrong_type, node_secret).status, 400);
    auto heartbeat = Json{{"node_id", "sfu-a"}, {"instance_id", "boot-other"}, {"participants", 0}};
    EXPECT_EQ(Post(*internal_routes, "/internal/v1/nodes/heartbeat", heartbeat, node_secret).status, 409);
}
TEST_F(ClusterApiTest, IdentityAndGrantsComeFromSessionAndMembership)
{
    meetings->RegisterNode({"sfu-a", "boot-a", "ws://localhost:30000/webrtc", 100, 0});
    EXPECT_EQ(Post(*public_routes, "/api/v1/rooms", Json::object()).status, 401);
    EXPECT_EQ(Post(*public_routes, "/api/v1/rooms", {{"owner", "mallory"}}, session).status, 400);
    auto room_reply = Post(*public_routes, "/api/v1/rooms", Json::object(), session);
    ASSERT_EQ(room_reply.status, 201);
    const auto room = Json::parse(room_reply.body).at("room_id").get<std::string>();
    auto admission = Post(*public_routes, "/api/v1/room-admissions", {{"room_id", room}}, session);
    ASSERT_EQ(admission.status, 200);
    const auto json = Json::parse(admission.body);
    service::auth::JwtAuthenticator verifier(std::make_shared<service::auth::StaticKeyProvider>(
        std::map<std::string, std::string>{{"test", jwt_secret}}));
    const auto token = verifier.Authenticate(json.at("token").get<std::string>(), service::auth::Audience::WebRtc);
    ASSERT_TRUE(token.Succeeded());
    EXPECT_EQ(token.context->subject_id, user);
    EXPECT_EQ(token.context->node_id, "sfu-a");
    EXPECT_EQ(token.context->instance_id, "boot-a");
    EXPECT_FALSE(token.context->assignment_id.empty());
    EXPECT_TRUE(token.context->video.can_publish);
    EXPECT_TRUE(token.context->video.room_admin);
    EXPECT_TRUE(token.context->video.can_update_own_metadata);
    EXPECT_EQ(Post(*public_routes, "/api/v1/room-admissions", {{"room_id", room}, {"can_publish", true}}, session).status, 400);
    EXPECT_EQ(Post(*public_routes, "/api/v1/room-admissions", {{"room_id", room}, {"user_id", "mallory"}}, session).status, 400);
    EXPECT_EQ(Post(*public_routes, "/api/v1/room-admissions", {{"room_id", "private-room"}}, session).status, 403);
    ASSERT_TRUE(accounts->Logout(session).Succeeded());
    EXPECT_EQ(Post(*public_routes, "/api/v1/room-admissions", {{"room_id", room}}, session).status, 401);
}
TEST_F(ClusterApiTest, OwnerManagementRoutesToAssignedNodeAndPropagatesFailures)
{
    meetings->RegisterNode({"sfu-a", "boot-a", "ws://localhost:30000/webrtc", 100, 0, "http://localhost:19080"});
    const auto response = Post(*public_routes, "/api/v1/rooms", Json::object(), session);
    const auto room = Json::parse(response.body).at("room_id").get<std::string>();
    const Json request{{"room_id", room}};
    auto empty = Post(*public_routes, "/api/v1/rooms/participants", request, session);
    ASSERT_EQ(empty.status, 200);
    EXPECT_TRUE(Json::parse(empty.body).at("participants").empty());
    EXPECT_TRUE(sent_command.is_null());
    const auto route = meetings->Admit(room, user, "Alice").assignment;
    EXPECT_EQ(Post(*public_routes, "/api/v1/rooms/participants", request).status, 401);
    ASSERT_TRUE(accounts->CreateAccount({"bob", "correct-password-123", "Bob"}).Succeeded());
    const auto bob = accounts->Login({"bob", "correct-password-123"}).value->session_token;
    EXPECT_EQ(Post(*public_routes, "/api/v1/rooms/participants", request, bob).status, 403);
    EXPECT_EQ(Post(*public_routes, "/api/v1/rooms/participants", request, session).status, 200);
    EXPECT_EQ(sent_route.assignment_id, route.assignment_id);
    EXPECT_EQ(sent_route.node.management_url, "http://localhost:19080");
    EXPECT_EQ(sent_command.value("type", ""), "participants");
    command_status = 503;
    EXPECT_EQ(Post(*public_routes, "/api/v1/rooms/participants/remove",
        {{"room_id", room}, {"participant_id", "target"}}, session).status, 503);
    EXPECT_EQ(sent_command.value("type", ""), "remove_participant");
    EXPECT_EQ(Post(*public_routes, "/api/v1/rooms/end", request, session).status, 503);
    EXPECT_EQ(Post(*public_routes, "/api/v1/room-admissions", request, session).status, 410);
    EXPECT_EQ(meetings->State(), service::ServiceState::Running);
    command_status = 200;
    EXPECT_EQ(Post(*public_routes, "/api/v1/rooms/end", request, session).status, 200);
    sent_command = nullptr;
    EXPECT_EQ(Post(*public_routes, "/api/v1/rooms/end", request, session).status, 200);
    EXPECT_TRUE(sent_command.is_null());
}
TEST_F(ClusterApiTest, ExistingRoutesRejectOperationsAfterModuleStopsAndCanRestart)
{
    meetings->RegisterNode({"sfu-a", "boot-a", "ws://localhost:30000/webrtc", 100, 0});
    services.StopAll();
    EXPECT_EQ(Post(*public_routes, "/api/v1/rooms", Json::object(), session).status, 503);
    EXPECT_EQ(Post(*internal_routes, "/internal/v1/nodes/heartbeat", {{"node_id", "sfu-a"},
        {"instance_id", "boot-a"}, {"participants", 0}}, node_secret).status, 503);
    EXPECT_FALSE(services.HealthSnapshot().front().healthy);
    ASSERT_TRUE(services.StartAll());
    EXPECT_EQ(Post(*public_routes, "/api/v1/rooms", Json::object(), session).status, 201);
    services.ShutdownAll();
    EXPECT_EQ(Post(*public_routes, "/api/v1/rooms", Json::object(), session).status, 503);
    EXPECT_EQ(meetings->State(), service::ServiceState::Created);
    ASSERT_TRUE(services.InitAll() && services.StartAll());
    EXPECT_EQ(Post(*public_routes, "/api/v1/rooms", Json::object(), session).status, 201);
}
class FakeTransport : public protocol::http::IHttpServer
{
public:
    bool Start(const protocol::http::HttpServerOptions&, RequestHandler callback) override { handler = std::move(callback); return true; }
    void Stop() noexcept override { handler = {}; }
    RequestHandler handler;
};
TEST(ClusterApiServer, InjectedRoutesRunWithoutAccountService)
{
    protocol::http::HttpServerOptions http; http.enabled = true;
    ApiRouter::Builder builder;
    builder.Get("/healthz", [](const ApiRequest&) { return JsonResponse(200, {{"status", "ok"}}); });
    const auto transport = std::make_shared<FakeTransport>();
    ApiServer api(http, builder.Build(), transport);
    ASSERT_TRUE(api.Start());
    std::promise<uint16_t> response;
    transport->handler({"GET", "/healthz", {}, {}}, [&](auto reply) { response.set_value(reply.status); });
    auto future = response.get_future();
    ASSERT_EQ(future.wait_for(std::chrono::seconds(2)), std::future_status::ready);
    EXPECT_EQ(future.get(), 200);
    api.Stop();
}
}
