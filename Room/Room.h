#ifndef _ROOM_H_
#define _ROOM_H_

#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

#include "RoomCommand.h"
#include "RoomTypes.h"

namespace room 
{
class Participant;

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

    bool Join(const std::shared_ptr<Participant>& participant);
    bool Leave(const std::string& participant_id);

    std::shared_ptr<Participant> GetParticipant(const std::string& participant_id) const;
    std::vector<std::shared_ptr<Participant>> GetParticipants() const;
    std::size_t ParticipantCount() const;

    // Room IDs may be global while an endpoint's source track ID is a local MID.
    bool PublishTrack(const std::string& participant_id, const media::MediaTrackPtr& track, uint32_t ssrc,
                      uint8_t payload_type, const std::string& source_track_id = {});

    bool UnpublishTrack(const std::string& track_id);

    bool SubscribeTrack(const std::string& subscriber_id, const std::string& track_id);

    bool UnsubscribeTrack(const std::string& subscriber_id, const std::string& track_id);

    std::vector<SubscriptionInfo> GetSubscriptionsByTrack(const std::string& track_id) const;
    std::vector<SubscriptionInfo> GetSubscriptionsBySubscriber(const std::string& subscriber_id) const;
    std::vector<std::shared_ptr<Participant>> GetSubscribers(const std::string& track_id) const;

    media::MediaTrackPtr ResolveTrackForSubscriber(const std::string& subscriber_id, const std::string& track_id);

    void Close();

    void SetOnParticipantChanged(std::function<void(std::shared_ptr<Participant>)> cb);
    void SetOnRoomClosed(std::function<void(const std::string& room_id)> cb);

private:
    bool CanJoinLocked(const std::shared_ptr<Participant>& participant) const;
    void UpdateStateLocked();
    bool SubscribeTrackLocked(const std::string& subscriber_id, const std::string& track_id);
    void UnsubscribeTrackLocked(const std::string& subscriber_id, const std::string& track_id);

private:
    mutable std::mutex mutex_;

    RoomInfo info_;
    RoomOptions options_;

    std::unordered_map<std::string, std::shared_ptr<Participant>> participants_;
    std::unordered_map<std::string, PublishedTrackInfo> published_tracks_;
    std::unordered_map<std::string, std::unordered_set<std::string>> track_subscribers_;
    std::unordered_map<std::string, std::unordered_set<std::string>> participant_subscriptions_;

    std::function<void(std::shared_ptr<Participant>)> on_participant_changed_;
    std::function<void(const std::string& room_id)> on_room_closed_;
};

class RoomManager
{
public:
    std::shared_ptr<Room>  GetOrCreateRoom(std::string room_id, std::string room_name);
    std::shared_ptr<Room> FindRoom(const std::string& room_id);
private:
    std::mutex mutex_;
    std::unordered_map<std::string, std::shared_ptr<Room>> rooms_;
};

class RoomCommandHandler 
{
public:
    explicit RoomCommandHandler(std::shared_ptr<RoomManager> manager)
        : room_manager_(std::move(manager))
    {
    }

    void Handle(const RoomCommand& cmd)
    {
        if (!room_manager_) return;
        auto room = (cmd.type == RoomCommandType::Join || cmd.type == RoomCommandType::Publish)
            ? room_manager_->GetOrCreateRoom(cmd.room_id, cmd.room_name)
            : room_manager_->FindRoom(cmd.room_id);
        if (!room) {
            return;
        }

        switch (cmd.type) {
        case RoomCommandType::Join:
            HandleJoin(room, cmd);
            break;
        case RoomCommandType::Publish:
            HandlePublish(room, cmd);
            break;
        case RoomCommandType::Subscribe:
            HandleSubscribe(room, cmd);
            break;
        case RoomCommandType::Leave:
            room->Leave(cmd.participant_id);
            break;
        case RoomCommandType::Unpublish:
            room->UnpublishTrack(cmd.track_id);
            break;
        case RoomCommandType::Unsubscribe:
            room->UnsubscribeTrack(cmd.participant_id, cmd.track_id);
            break;
        case RoomCommandType::Close:
            room->Close();
            break;
        default:
            break;
        }
    }

private:
    void HandleJoin(std::shared_ptr<Room>&, const RoomCommand& cmd);
    void HandlePublish(std::shared_ptr<Room>&, const RoomCommand& cmd);
    void HandleSubscribe(std::shared_ptr<Room>&, const RoomCommand& cmd);

private:
    std::shared_ptr<RoomManager> room_manager_;
};

}



#endif /* _ROOM_H_ */
