#ifndef _ROOM_H_
#define _ROOM_H_

#include <memory>
#include <mutex>
#include <string>
#include <functional>

#include "TrackeInfo.h"
#include "Participant.h"

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


class Room : public std::enable_shared_from_this<Room>
{
public:
    using Ptr = std::shared_ptr<Room>;

    explicit Room(RoomInfo info, RoomOptions options = {});

    ~Room();

    const std::string& Id() const;
    const std::string& Name() const;

    RoomInfo GetInfo() const;
    RoomState State() const;

    bool IsClosed() const;

    bool Join(const Participant::Ptr& participant);
    bool Leave(const std::string& participant_id);

    Participant::Ptr GetParticipant(const std::string& participant_id) const;
    std::vector<Participant::Ptr> GetParticipants() const;
    size_t ParticipantCount() const;

    bool PublishTrack(const std::string& participant_id,
                      const media::MediaTrackPtr& track,
                      uint32_t ssrc,
                      uint8_t payload_type);

    bool UnpublishTrack(const std::string& track_id);

    media::MediaTrackPtr ResolveTrackForSubscriber(const std::string& subscriber_id,
                                                    const std::string& track_id);

    void Close();

    void SetOnParticipantChanged(std::function<void(Participant::Ptr)> cb);
    void SetOnRoomClosed(std::function<void(const std::string& room_id)> cb);

private:
    bool CanJoinLocked(const Participant::Ptr& participant) const;
    void UpdateStateLocked();

private:
    mutable std::mutex mutex_;

    RoomInfo info_;
    RoomOptions options_;

    std::unordered_map<std::string, Participant::Ptr> participants_;

    std::function<void(Participant::Ptr)> on_participant_changed_;
    std::function<void(const std::string& room_id)> on_room_closed_;
};
}



#endif /* _ROOM_H_ */