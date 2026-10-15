#include "server/cluster/ClusterController.h"
#include "server/api/ApiServer.h"
#include "server/api/ApiResponse.h"
#include "service/Account/ScryptPasswordHasher.h"
#include "service/Account/SqliteAccountStore.h"
#include "service/Auth/JwtAuthenticator.h"
#include "service/Auth/KeyProvider.h"
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
    std::shared_ptr<service::cluster::Coordinator> coordinator;
    std::unique_ptr<ApiRouter> public_routes, internal_routes;
    std::string session, user;
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
        coordinator = std::make_shared<service::cluster::Coordinator>(":memory:");
        server::cluster::ClusterController controller(accounts, coordinator, {{"sfu-a", node_secret}}, "test", jwt_secret);
        ApiRouter::Builder public_builder, internal_builder;
        controller.RegisterPublic(public_builder); controller.RegisterInternal(internal_builder);
        public_routes = std::make_unique<ApiRouter>(public_builder.Build());
        internal_routes = std::make_unique<ApiRouter>(internal_builder.Build());
    }
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
    coordinator->Register({"sfu-a", "boot-a", "ws://localhost:30000/webrtc", 100, 0});
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
    EXPECT_TRUE(token.context->video.can_publish);
    EXPECT_EQ(Post(*public_routes, "/api/v1/room-admissions", {{"room_id", room}, {"can_publish", true}}, session).status, 400);
    EXPECT_EQ(Post(*public_routes, "/api/v1/room-admissions", {{"room_id", room}, {"user_id", "mallory"}}, session).status, 400);
    EXPECT_EQ(Post(*public_routes, "/api/v1/room-admissions", {{"room_id", "private-room"}}, session).status, 403);
    ASSERT_TRUE(accounts->Logout(session).Succeeded());
    EXPECT_EQ(Post(*public_routes, "/api/v1/room-admissions", {{"room_id", room}}, session).status, 401);
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
