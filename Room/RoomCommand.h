#ifndef PACKETIA_ROOM_ROOM_COMMAND_H
#define PACKETIA_ROOM_ROOM_COMMAND_H

#include <cstdint>
#include <string>

#include "MediaTrackFwd.h"

namespace room
{

// A command describes an operation; dispatch and resource access belong to the
// command handler rather than the message data.
enum class RoomCommandType
{
    Join,
    Leave,
    Publish,
    Subscribe,
    Unpublish,
    Unsubscribe,
    Close,
};

struct RoomCommand
{
    RoomCommandType type;
    std::string room_id;
    std::string room_name;
    std::string conn_id;
    std::string participant_id;
    std::string track_id;
    media::MediaTrackPtr track;
    uint32_t ssrc = 0;
    uint8_t payload_type = 0;
};

} // namespace room

#endif
