#pragma once
#include <cstdint>
#include <stdexcept>
#include <string>
namespace service::cluster
{
struct Failure : std::runtime_error
{
    Failure(uint16_t status, const char* code) : std::runtime_error(code), status(status), code(code) {}
    uint16_t status;
    const char* code;
};
struct Node
{
    std::string id, instance, signaling_url;
    int max_rooms = 0;
    int participants = 0;
    std::string management_url;
};
struct RoomAssignment
{
    std::string room_id, assignment_id;
    Node node;
    int64_t reclaim_after = 0;
    bool closing = false;
};
}
