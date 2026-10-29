#include "RoomController.h"
#include "ControlApiUtil.h"
namespace server::meeting
{
using namespace control_api;
void RoomController::RegisterRoutes(api::ApiRouter::Builder& builder) const
{
    const auto accounts = accounts_;
    const auto meetings = meetings_;
    const auto caller = [accounts](const ApiRequest& request) {
        const auto identity = accounts->ResolveSession(Bearer(request));
        if (!identity.Succeeded()) throw Failure(401, "unauthenticated");
        return *identity.value;
    };
    builder.PostJson("/api/v1/rooms", Guard([meetings, caller](const ApiRequest& request) {
        const auto identity = caller(request); Fields(request.json, {});
        return JsonResponse(201, {{"room_id", meetings->CreateRoom(identity.user_id)}});
    }));
    builder.PostJson("/api/v1/room-members", Guard([meetings, caller](const ApiRequest& request) {
        const auto identity = caller(request);
        Fields(request.json, {"room_id", "user_id", "role"});
        const auto role = String(request.json, "role");
        if (role != "speaker" && role != "listener") throw Failure(400, "invalid_role");
        meetings->SetMemberRole(String(request.json, "room_id"), identity.user_id, String(request.json, "user_id"), role == "speaker");
        return JsonResponse(200, {{"status", "ok"}, {"effective", "next_join"}});
    }));
    builder.PostJson("/api/v1/room-admissions", Guard([meetings, caller](const ApiRequest& request) {
        const auto identity = caller(request); Fields(request.json, {"room_id"});
        const auto room = String(request.json, "room_id");
        const auto admission = meetings->Admit(room, identity.user_id, identity.display_name);
        const auto& route = admission.assignment;
        return JsonResponse(200, {{"room_id", room}, {"node_id", route.node.id}, {"instance_id", route.node.instance},
            {"assignment_id", route.assignment_id}, {"signaling_url", route.node.signaling_url},
            {"token", admission.token}, {"expires_at", admission.expires_at}});
    }));
    const auto management = [&](const char* path, const char* type) {
        builder.PostJson(path, Guard([meetings, caller, type = std::string(type)](const ApiRequest& request) {
            const auto identity = caller(request);
            if (type == "participants") Fields(request.json, {"room_id", "limit", "cursor", "revision"});
            else if (type == "participant" || type == "remove_participant") Fields(request.json, {"room_id", "participant_id"});
            else Fields(request.json, {"room_id", "participant_id", "name", "metadata", "attributes", "permissions"});
            const auto room = String(request.json, "room_id");
            auto command = request.json; command.erase("room_id"); command["type"] = type;
            if (type != "participants") String(command, "participant_id");
            const auto response = meetings->ManageRoom(room, identity.user_id, std::move(command));
            return JsonResponse(response.status, response.body);
        }));
    };
    management("/api/v1/rooms/participants", "participants");
    management("/api/v1/rooms/participant", "participant");
    management("/api/v1/rooms/participants/update", "update_participant");
    management("/api/v1/rooms/participants/remove", "remove_participant");
    builder.PostJson("/api/v1/rooms/end", Guard([meetings, caller](const ApiRequest& request) {
        const auto identity = caller(request); Fields(request.json, {"room_id"});
        const auto response = meetings->EndRoom(String(request.json, "room_id"), identity.user_id);
        return JsonResponse(response.status, response.body);
    }));
}
}
