#include "ClusterController.h"
#include "server/api/ApiResponse.h"
#include "service/Auth/JoinToken.h"
#include <openssl/crypto.h>
#include <algorithm>
#include <cctype>
#include <set>

namespace server::cluster
{
namespace
{
using namespace server::api;
using service::cluster::Coordinator;
using service::cluster::Failure;
using Json = nlohmann::json;
using Response = protocol::http::HttpResponse;
std::string Bearer(const ApiRequest& request)
{
    const auto it = request.http.headers.find("authorization");
    if (it == request.http.headers.end() || it->second.size() < 8 || it->second.size() > 4103) return {};
    auto prefix = it->second.substr(0, 7);
    std::transform(prefix.begin(), prefix.end(), prefix.begin(), [](unsigned char c) { return std::tolower(c); });
    return prefix == "bearer " ? it->second.substr(7) : "";
}
std::string String(const Json& json, const char* name, std::size_t limit = 128)
{
    const auto it = json.find(name);
    if (it == json.end() || !it->is_string()) throw Failure(400, "invalid_request");
    auto value = it->get<std::string>();
    if (value.empty() || value.size() > limit) throw Failure(400, "invalid_request");
    return value;
}
int Number(const Json& json, const char* name, int low, int high)
{
    const auto it = json.find(name);
    if (it == json.end() || !it->is_number_integer()) throw Failure(400, "invalid_request");
    if (*it < low || *it > high) throw Failure(400, "invalid_request");
    return it->get<int>();
}
void Fields(const Json& json, std::initializer_list<const char*> names)
{
    std::set<std::string> allowed(names.begin(), names.end());
    for (auto it = json.begin(); it != json.end(); ++it)
        if (!allowed.count(it.key())) throw Failure(400, "unknown_field");
}
ApiRouter::Handler Guard(ApiRouter::Handler handler)
{
    return [handler = std::move(handler)](const ApiRequest& request) -> Response {
        try { return handler(request); }
        catch (const Failure& error) { return ErrorResponse(error.status, error.code, "Cluster operation rejected."); }
    };
}
}
struct ClusterController::State
{
    std::shared_ptr<service::account::AccountService> accounts;
    std::shared_ptr<Coordinator> coordinator;
    std::map<std::string, std::string> node_secrets;
    std::string issuer, signing_secret;
    service::account::AccountIdentity Caller(const ApiRequest& request) const
    {
        const auto caller = accounts->ResolveSession(Bearer(request));
        if (!caller.Succeeded()) throw Failure(401, "unauthenticated");
        return *caller.value;
    }
    std::string NodeCaller(const ApiRequest& request) const
    {
        const auto id = String(request.json, "node_id");
        const auto found = node_secrets.find(id);
        const auto bearer = Bearer(request);
        if (found == node_secrets.end() || bearer.size() != found->second.size() ||
            CRYPTO_memcmp(bearer.data(), found->second.data(), bearer.size()) != 0)
            throw Failure(401, "unauthenticated_node");
        return id;
    }
};
ClusterController::ClusterController(std::shared_ptr<service::account::AccountService> accounts,
    std::shared_ptr<Coordinator> coordinator, std::map<std::string, std::string> node_secrets,
    std::string issuer, std::string signing_secret)
    : state_(std::make_shared<State>(State{std::move(accounts), std::move(coordinator),
        std::move(node_secrets), std::move(issuer), std::move(signing_secret)})) {}

void ClusterController::RegisterPublic(ApiRouter::Builder& builder) const
{
    const auto state = state_;
    builder.Get("/healthz", [](const ApiRequest&) { return JsonResponse(200, {{"status", "ok"}, {"role", "control"}}); });
    builder.PostJson("/api/v1/rooms", Guard([state](const ApiRequest& request) {
        const auto caller = state->Caller(request);
        Fields(request.json, {});
        return JsonResponse(201, {{"room_id", state->coordinator->CreateRoom(caller.user_id)}});
    }));
    builder.PostJson("/api/v1/room-members", Guard([state](const ApiRequest& request) {
        const auto caller = state->Caller(request);
        Fields(request.json, {"room_id", "user_id", "role"});
        const auto role = String(request.json, "role");
        if (role != "speaker" && role != "listener") throw Failure(400, "invalid_role");
        state->coordinator->AddMember(String(request.json, "room_id"), caller.user_id,
            String(request.json, "user_id"), role == "speaker");
        return JsonResponse(200, {{"status", "ok"}});
    }));
    builder.PostJson("/api/v1/room-admissions", Guard([state](const ApiRequest& request) {
        const auto caller = state->Caller(request);
        Fields(request.json, {"room_id"});
        const auto room = String(request.json, "room_id");
        const auto admission = state->coordinator->Admit(room, caller.user_id);
        service::auth::AuthContext context;
        context.subject_id = caller.user_id;
        context.name = caller.display_name;
        context.issuer = state->issuer;
        context.token_id = Coordinator::RandomId();
        const auto now = Coordinator::UnixSeconds();
        context.expires_at_unix_seconds = now + 60;
        context.video = {room, true, admission.can_publish, true};
        context.node_id = admission.node.id;
        context.instance_id = admission.node.instance;
        return JsonResponse(200, {{"room_id", room}, {"node_id", context.node_id},
            {"instance_id", context.instance_id}, {"signaling_url", admission.node.signaling_url},
            {"token", service::auth::SignJoinToken(context, state->signing_secret, now)},
            {"expires_at", context.expires_at_unix_seconds}});
    }));
}
void ClusterController::RegisterInternal(ApiRouter::Builder& builder) const
{
    const auto state = state_;
    builder.PostJson("/internal/v1/nodes/register", Guard([state](const ApiRequest& request) {
        const auto id = state->NodeCaller(request);
        Fields(request.json, {"node_id", "instance_id", "signaling_url", "max_rooms", "participants"});
        state->coordinator->Register({id, String(request.json, "instance_id"),
            String(request.json, "signaling_url", 1024), Number(request.json, "max_rooms", 1, 10000),
            Number(request.json, "participants", 0, 65536)});
        return JsonResponse(200, {{"lease_seconds", Coordinator::kLeaseSeconds}});
    }));
    builder.PostJson("/internal/v1/nodes/heartbeat", Guard([state](const ApiRequest& request) {
        const auto id = state->NodeCaller(request);
        Fields(request.json, {"node_id", "instance_id", "participants"});
        state->coordinator->Heartbeat(id, String(request.json, "instance_id"),
            Number(request.json, "participants", 0, 65536));
        return JsonResponse(200, {{"lease_seconds", Coordinator::kLeaseSeconds}});
    }));
}
}
