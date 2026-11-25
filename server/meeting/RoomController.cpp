#include "RoomController.h"
#include "ControlApiUtil.h"
namespace server::meeting
{
using namespace control_api;
void RoomController::RegisterRoutes(api::ApiRouter::Builder& builder) const
{
    const auto controller = *this;
    builder.PostJson("/api/v1/rooms", Guard([controller](const ApiRequest& request) {
        return controller.CreateRoom(request);
    }));
    builder.PostJson("/api/v1/room-members", Guard([controller](const ApiRequest& request) {
        return controller.SetMemberRole(request);
    }));
    builder.PostJson("/api/v1/room-admissions", Guard([controller](const ApiRequest& request) {
        return controller.AdmitRoom(request);
    }));
    const auto management = [&](const char* path, const char* type) {
        builder.PostJson(path, Guard([controller, type = std::string(type)](const ApiRequest& request) {
            return controller.ManageParticipants(request, type);
        }));
    };
    management("/api/v1/rooms/participants", "participants");
    management("/api/v1/rooms/participant", "participant");
    management("/api/v1/rooms/participants/update", "update_participant");
    management("/api/v1/rooms/participants/remove", "remove_participant");
    builder.PostJson("/api/v1/rooms/end", Guard([controller](const ApiRequest& request) {
        return controller.EndRoom(request);
    }));
}

service::account::AccountIdentity RoomController::ResolveCaller(const ApiRequest& request) const
{
    const auto identity = accounts_->ResolveSession(Bearer(request));
    if (!identity.Succeeded()) throw Failure(401, "unauthenticated");
    return *identity.value;
}

protocol::http::HttpResponse RoomController::CreateRoom(const ApiRequest& request) const
{
    const auto identity = ResolveCaller(request);
    Fields(request.json, {});
    if (auto rejection = policy_->CheckCreateRoom(identity)) return std::move(*rejection);
    return JsonResponse(201, {{"room_id", meetings_->CreateRoom(identity.user_id)}});
}

protocol::http::HttpResponse RoomController::SetMemberRole(const ApiRequest& request) const
{
    const auto identity = ResolveCaller(request);
    Fields(request.json, {"room_id", "user_id", "role"});
    const auto role = String(request.json, "role");
    if (role != "speaker" && role != "listener") throw Failure(400, "invalid_role");
    meetings_->SetMemberRole(String(request.json, "room_id"), identity.user_id,
        String(request.json, "user_id"), role == "speaker");
    return JsonResponse(200, {{"status", "ok"}, {"effective", "next_join"}});
}

protocol::http::HttpResponse RoomController::AdmitRoom(const ApiRequest& request) const
{
    const auto identity = ResolveCaller(request);
    Fields(request.json, {"room_id"});
    const auto room = String(request.json, "room_id");

    if (auto rejection = policy_->CheckRoomAdmission(identity, room)) return std::move(*rejection);

    const auto admission = meetings_->Admit(room, identity.user_id, identity.display_name);
    const auto& route = admission.assignment;
    return JsonResponse(200, {{"room_id", room}, {"node_id", route.node.id}, {"instance_id", route.node.instance},
        {"assignment_id", route.assignment_id}, {"signaling_url", route.node.signaling_url},
        {"token", admission.token}, {"expires_at", admission.expires_at}});
}

protocol::http::HttpResponse RoomController::ManageParticipants(const ApiRequest& request, const std::string& type) const
{
    const auto identity = ResolveCaller(request);
    if (type == "participants") Fields(request.json, {"room_id", "limit", "cursor", "revision"});
    else if (type == "participant" || type == "remove_participant") Fields(request.json, {"room_id", "participant_id"});
    else Fields(request.json, {"room_id", "participant_id", "name", "metadata", "attributes", "permissions"});
    const auto room = String(request.json, "room_id");
    auto command = request.json;
    command.erase("room_id");
    command["type"] = type;
    if (type != "participants") String(command, "participant_id");
    const auto response = meetings_->ManageRoom(room, identity.user_id, std::move(command));
    return JsonResponse(response.status, response.body);
}

protocol::http::HttpResponse RoomController::EndRoom(const ApiRequest& request) const
{
    const auto identity = ResolveCaller(request);
    Fields(request.json, {"room_id"});
    const auto response = meetings_->EndRoom(String(request.json, "room_id"), identity.user_id);
    return JsonResponse(response.status, response.body);
}
}
