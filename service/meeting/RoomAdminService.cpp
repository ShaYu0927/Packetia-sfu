#include "RoomAdminService.h"
namespace service::meeting
{
using Json = nlohmann::json;
RoomCommandResult RoomAdminService::Execute(const std::string& room, const std::string& owner, Json command)
{
    meetings_->RequireOwner(room, owner);
    const auto route = nodes_->FindRoom(room);
    if (!route)
    {
        if (command.value("type", "") == "participants")
            return {200, {{"type", "participants"}, {"room_id", room}, {"participants", Json::array()},
                {"revision", 0}, {"total", 0}, {"next_cursor", ""}}};
        throw cluster::Failure(404, "room_not_running");
    }
    return sender_(*route, command);
}
RoomCommandResult RoomAdminService::EndRoom(const std::string& room, const std::string& owner)
{
    if (meetings_->BeginEnd(room, owner)) return {200, {{"type", "room_ended"}, {"room_id", room}}};
    const auto route = nodes_->FindRoom(room, true);
    if (!route) throw cluster::Failure(503, "room_end_pending");
    auto result = sender_(*route, {{"type", "end_room"}});
    if (result.status == 200)
        nodes_->ReleaseRoom(route->node.id, route->node.instance, room, route->assignment_id);
    return result;
}
}
