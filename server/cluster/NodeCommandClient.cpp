#include "NodeCommandClient.h"
#include "ControlHttpClient.h"
namespace server::cluster
{
service::meeting::RoomCommandSender MakeNodeCommandSender(std::map<std::string, std::string> credentials)
{
    return [credentials = std::move(credentials)](const service::cluster::RoomAssignment& route, const nlohmann::json& command) {
        using Result = service::meeting::RoomCommandResult;
        const auto found = credentials.find(route.node.id);
        if (found == credentials.end() || !service::cluster::Coordinator::ValidManagementUrl(route.node.management_url))
            return Result{503, {{"type", "error"}, {"code", "node_management_unavailable"}}};
        const auto reply = PostControlJson(route.node.management_url + "/internal/v1/rooms/command", found->second,
            {{"node_id", route.node.id}, {"instance_id", route.node.instance}, {"room_id", route.room_id},
             {"assignment_id", route.assignment_id}, {"command", command}}, 2000);
        if (!reply.status) return Result{503, {{"type", "error"}, {"code", "node_command_timeout"},
            {"error", "Node response unavailable; execution may have started"}}};
        return Result{reply.status, reply.body};
    };
}
}
