#pragma once
#include "MeetingService.h"
#include "Coordinator.h"
#include "third/nlohmann/json.hpp"
namespace service::meeting
{
struct RoomCommandResult
{
    uint16_t status = 503;
    nlohmann::json body;
};
using RoomCommandSender = std::function<RoomCommandResult(const cluster::RoomAssignment&, const nlohmann::json&)>;
class RoomAdminService final
{
public:
    RoomAdminService(std::shared_ptr<MeetingService> meetings, std::shared_ptr<cluster::Coordinator> nodes, RoomCommandSender sender)
        : meetings_(std::move(meetings)), nodes_(std::move(nodes)), sender_(std::move(sender)) {}
    RoomCommandResult Execute(const std::string& room, const std::string& owner, nlohmann::json command);
    RoomCommandResult EndRoom(const std::string& room, const std::string& owner);
private:
    std::shared_ptr<MeetingService> meetings_;
    std::shared_ptr<cluster::Coordinator> nodes_;
    RoomCommandSender sender_;
};
}
