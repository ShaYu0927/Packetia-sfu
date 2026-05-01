#ifndef _PARTICIPANT_H_
#define _PARTICIPANT_H_

#include "Room.h"
#include <functional>
#include <memory>

namespace room 
{
class LocalParticipant 
{
public:
    virtual ~LocalParticipant() = default;

    virtual ParticipantId Id() const = 0;
    virtual ParticipantIdentity Identity() const = 0;
    virtual ParticipantState State() const = 0;

    virtual bool IsReady() const = 0;
    virtual bool IsPublisher() const = 0;
    virtual bool Hidden() const = 0;
    virtual bool IsClosed() const = 0;

    virtual std::vector<std::shared_ptr<MediaTrack>> GetPublishedTracks() const = 0;

    virtual void SubscribeToTrack(const TrackId& trackId, bool sync) = 0;
    virtual void UnsubscribeFromTrack(const TrackId& trackId) = 0;

    virtual void SendJoinResponse(const RoomInfo& roomInfo) = 0;
    virtual void SendParticipantUpdate(const ParticipantInfo& info) = 0;
    virtual void SendRoomUpdate(const RoomInfo& roomInfo) = 0;
    virtual void Close(bool notify, CloseReason reason) = 0;

    using StateChangeCallback = std::function<void(std::shared_ptr<LocalParticipant>)>;

    using TrackPublishedCallback = std::function<void(std::shared_ptr<LocalParticipant>, std::shared_ptr<MediaTrack>)>;

    using TrackUnpublishedCallback = std::function<void(std::shared_ptr<LocalParticipant>, std::shared_ptr<MediaTrack>)>;

    using LeaveCallback = std::function<void(std::shared_ptr<LocalParticipant>, CloseReason)>;

    virtual void OnStateChange(StateChangeCallback cb) = 0;
    virtual void OnTrackPublished(TrackPublishedCallback cb) = 0;
    virtual void OnTrackUnpublished(TrackUnpublishedCallback cb) = 0;
    virtual void OnLeave(LeaveCallback cb) = 0;
};

}



#endif // _PARTICIPANT_H_