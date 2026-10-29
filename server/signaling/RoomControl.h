#pragma once
#include "SignalMessages.h"
#include "BoundedChannel.h"
#include <atomic>
#include <future>
namespace server::signaling
{
struct RoomRelease { std::string room_id, assignment_id; };
struct RoomControlRequest
{
    std::string room_id, assignment_id;
    Json command;
    uint64_t deadline_ms = 0;
    std::shared_ptr<std::atomic<bool>> cancelled;
    std::shared_ptr<std::promise<Json>> result;
};
using RoomControlChannel = utils::BoundedChannel<RoomControlRequest>;
}
