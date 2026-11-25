#include "AgentController.h"
#include "ControlApiUtil.h"

namespace server::api
{
using namespace control_api;
namespace
{
Json Describe(const service::ai::AgentSession& session)
{
    return {{"session_id", session.session_id}, {"room_id", session.room_id},
            {"state", session.state == service::ai::AgentSessionState::Prepared ? "prepared" : "stopped"}};
}
}

void AgentController::RegisterRoutes(ApiRouter::Builder& builder) const
{
    const auto controller = *this;
    builder.PostJson("/api/v1/agent-sessions", Guard([controller](const ApiRequest& request) {
        return controller.Create(request);
    }));
    builder.PostJson("/api/v1/agent-sessions/get", Guard([controller](const ApiRequest& request) {
        return controller.Get(request);
    }));
    builder.PostJson("/api/v1/agent-sessions/stop", Guard([controller](const ApiRequest& request) {
        return controller.Stop(request);
    }));
}

std::string AgentController::ResolveOwner(const ApiRequest& request) const
{
    const auto identity = accounts_->ResolveSession(Bearer(request));
    if (!identity.Succeeded()) throw Failure(401, "unauthenticated");
    return identity.value->user_id;
}

Response AgentController::Create(const ApiRequest& request) const
{
    const auto owner = ResolveOwner(request);
    Fields(request.json, {"room_id"});
    return JsonResponse(201, Describe(agents_->CreateSession(String(request.json, "room_id"), owner)));
}

Response AgentController::Get(const ApiRequest& request) const
{
    const auto owner = ResolveOwner(request);
    Fields(request.json, {"session_id"});
    return JsonResponse(200, Describe(agents_->GetSession(String(request.json, "session_id"), owner)));
}

Response AgentController::Stop(const ApiRequest& request) const
{
    const auto owner = ResolveOwner(request);
    Fields(request.json, {"session_id"});
    return JsonResponse(200, Describe(agents_->StopSession(String(request.json, "session_id"), owner)));
}
}
