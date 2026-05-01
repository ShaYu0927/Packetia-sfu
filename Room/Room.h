#ifndef _ROOM_H_
#define _ROOM_H_

#include <cstdint>
#include <string>


namespace room 
{
using RoomId = std::string;
using RoomName = std::string;
using ParticipantId = std::string;
using ParticipantIdentity = std::string;
using TrackId = std::string;

enum class ParticipantState 
{
    Joining,
    Active,
    Disconnected,
};

enum class CloseReason
{
    Unknown,
    RoomClosed,
    ParticipantLeft,
    JoinTimeout,
    DuplicateIdentity,
};

struct ParticipantInfo 
{
    ParticipantId id;
    ParticipantIdentity identity;
    ParticipantState state = ParticipantState::Joining;
    bool isPublisher = false;
    bool hidden = false;
};

struct RoomInfo 
{
    RoomId id;
    RoomName name;

    uint32_t maxParticipants = 0;
    uint32_t numParticipants = 0;
    uint32_t numPublishers = 0;

    uint32_t emptyTimeoutSec = 300;
    uint32_t departureTimeoutSec = 20;
};


struct ParticipantOptions 
{
    bool autoSubscribe = true;
};

enum class MediaKind 
{
    Audio,
    Video,
};

enum class TrackState 
{
    New,
    Active,
    Muted,
    Ended,
};

class MediaTrack 
{
public:
    MediaTrack(std::string trackId, MediaKind kind, std::string publisherId)
        : trackId_(std::move(trackId)),
          kind_(kind),
          publisherId_(std::move(publisherId)) {}

    const std::string& Id() const 
    {
        return trackId_;
    }

    MediaKind Kind() const 
    {
        return kind_;
    }

    const std::string& PublisherId() const 
    {
        return publisherId_;
    }

    TrackState State() const 
    {
        return state_;
    }

    void SetState(TrackState state) 
    {
        state_ = state;
    }

private:
    std::string trackId_;      // track 唯一 ID
    MediaKind kind_;           // audio / video
    std::string publisherId_;  // 发布的这个 track
    TrackState state_ = TrackState::New;
};

}



#endif /* _ROOM_H_ */