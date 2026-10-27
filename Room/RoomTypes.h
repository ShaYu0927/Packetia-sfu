#ifndef PACKETIA_ROOM_ROOM_TYPES_H
#define PACKETIA_ROOM_ROOM_TYPES_H

#include <cstdint>
#include <string>

#include "MediaTrackFwd.h"

namespace room
{

enum class RoomState
{
    Created = 0,
    Active,
    Empty,
    Closed,
};

struct RoomOptions
{
    uint32_t max_participants = 0;
    uint32_t empty_timeout_sec = 60;
    uint32_t departure_timeout_sec = 30;
    bool auto_subscribe = true;
};

struct RoomInfo
{
    std::string room_id;
    std::string room_name;
    uint64_t created_at_ms = 0;
    uint64_t first_joined_at_ms = 0;
    uint64_t last_left_at_ms = 0;
    uint32_t participant_count = 0;
    RoomState state = RoomState::Created;
};

struct PublishedTrackInfo
{
    uint64_t publication_id = 0;
    std::string publisher_id;
    std::string source_track_id;
    media::MediaTrackPtr track;
    uint32_t ssrc = 0;
    uint8_t payload_type = 0;
};

struct SubscriptionInfo
{
    std::string subscriber_id;
    std::string publisher_id;
    std::string track_id;
};

} // namespace room

#endif
