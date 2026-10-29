#include "RoomControlController.h"
#include "ControlApiUtil.h"
namespace server::meeting
{
using namespace control_api;
void RoomControlController::RegisterRoutes(api::ApiRouter::Builder& builder) const
{
    const auto node = node_; const auto credential = credential_; const auto lease = lease_; const auto rtc = rtc_;
    builder.PostJson("/internal/v1/rooms/command", Guard([node, credential, lease, rtc](const ApiRequest& request) {
        if (!MatchesCredential(Bearer(request), credential)) throw Failure(401, "unauthenticated_control");
        Fields(request.json, {"node_id", "instance_id", "room_id", "assignment_id", "command"});
        if (String(request.json, "node_id") != node.id || String(request.json, "instance_id") != node.instance)
            throw Failure(409, "instance_fenced");
        if (!lease->IsLive()) throw Failure(503, "node_lease_expired");
        const auto& command = request.json.at("command");
        if (!command.is_object() || !command.contains("type") || !command["type"].is_string()) throw Failure(400, "invalid_command");
        const auto type = command["type"].get<std::string>();
        if (type != "participants" && type != "participant" && type != "update_participant" && type != "remove_participant" && type != "end_room")
            throw Failure(400, "invalid_command");
        auto result = rtc->ControlRoom(String(request.json, "room_id"), String(request.json, "assignment_id"), command);
        uint16_t status = 200;
        if (result.value("type", "") == "error")
        {
            const auto code = result.value("code", "");
            status = code == "unavailable" ? 503 : code == "stale_assignment" ? 409 : code == "participant_not_found" ? 404 : 400;
        }
        return JsonResponse(status, std::move(result));
    }));
}
}
