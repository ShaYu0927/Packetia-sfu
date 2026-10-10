#ifndef PACKETIA_ROOM_PARTICIPANT_TYPES_H_
#define PACKETIA_ROOM_PARTICIPANT_TYPES_H_

#include <cstddef>
#include <cstdint>
#include <string>

namespace room
{

enum class ParticipantState
{
    Joining = 0,
    Joined,
    Active,
    Disconnected,
    Reconnecting,
};

struct ParticipantIdentity
{
    std::string participant_id;
    std::string name;
    // Set from a verified authentication result; empty for anonymous users.
    std::string subject_id;
};

struct SignalingBinding
{
    std::string connection_id;
    // Changes on every bind, including reuse of the same connection ID.
    uint64_t generation = 0;
};

// A value snapshot, containing no live protocol objects or credentials.
struct ParticipantInfo
{
    ParticipantIdentity identity;
    ParticipantState state = ParticipantState::Joining;
    SignalingBinding signaling;
    uint64_t media_session_id = 0;
    size_t published_track_count = 0;
    size_t subscribed_track_count = 0;
};

} // namespace room

#endif // PACKETIA_ROOM_PARTICIPANT_TYPES_H_
