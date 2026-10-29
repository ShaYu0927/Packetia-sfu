#include "NodeController.h"
#include "ControlApiUtil.h"
namespace server::cluster
{
using namespace control_api;
void NodeController::RegisterRoutes(api::ApiRouter::Builder& builder) const
{
    const auto meetings = meetings_;
    const auto secrets = secrets_;
    const auto caller = [secrets](const ApiRequest& request) {
        const auto id = String(request.json, "node_id");
        const auto found = secrets.find(id);
        if (found == secrets.end() || !MatchesCredential(Bearer(request), found->second)) throw Failure(401, "unauthenticated_node");
        return id;
    };
    const auto leaseReply = [](const std::vector<service::cluster::RoomAssignment>& rooms) {
        auto candidates = Json::array();
        for (const auto& room : rooms)
            candidates.push_back({{"room_id", room.room_id}, {"assignment_id", room.assignment_id}, {"closing", room.closing}});
        return JsonResponse(200, {{"lease_seconds", service::cluster::Coordinator::kLeaseSeconds}, {"reclaim", candidates}});
    };
    builder.PostJson("/internal/v1/nodes/register", Guard([meetings, caller, leaseReply](const ApiRequest& request) {
        const auto id = caller(request);
        Fields(request.json, {"node_id", "instance_id", "signaling_url", "management_url", "max_rooms", "participants"});
        const auto instance = String(request.json, "instance_id");
        const auto management = request.json.contains("management_url") ? String(request.json, "management_url", 1024) : "";
        return leaseReply(meetings->RegisterNode({id, instance, String(request.json, "signaling_url", 1024),
            Number(request.json, "max_rooms", 1, 10000), Number(request.json, "participants", 0, 65536), management}));
    }));
    builder.PostJson("/internal/v1/nodes/heartbeat", Guard([meetings, caller, leaseReply](const ApiRequest& request) {
        const auto id = caller(request);
        Fields(request.json, {"node_id", "instance_id", "participants"});
        const auto instance = String(request.json, "instance_id");
        return leaseReply(meetings->HeartbeatNode(id, instance, Number(request.json, "participants", 0, 65536)));
    }));
    builder.PostJson("/internal/v1/rooms/release", Guard([meetings, caller](const ApiRequest& request) {
        const auto id = caller(request);
        Fields(request.json, {"node_id", "instance_id", "room_id", "assignment_id"});
        const bool released = meetings->ReleaseRoom(id, String(request.json, "instance_id"), String(request.json, "room_id"), String(request.json, "assignment_id"));
        return JsonResponse(200, {{"released", released}});
    }));
}
}
