#include "Participant.h"

#include <utility>

namespace room
{

Participant::Participant(std::string participant_id, std::string name)
    : participant_id_(std::move(participant_id)),
      name_(std::move(name))
{
}

Participant::~Participant() = default;

std::string Participant::Id() const
{
    std::lock_guard<std::mutex> lock(mutex_);
    return participant_id_;
}

std::string Participant::Name() const
{
    std::lock_guard<std::mutex> lock(mutex_);
    return name_;
}

void Participant::SetName(const std::string& name)
{
    std::lock_guard<std::mutex> lock(mutex_);
    name_ = name;
}

void Participant::BindSession(std::shared_ptr<MediaSession> session)
{
    std::lock_guard<std::mutex> lock(mutex_);
    session_ = std::move(session);
}

std::shared_ptr<MediaSession> Participant::GetSession() const
{
    std::lock_guard<std::mutex> lock(mutex_);
    return session_;
}

void Participant::SetState(ParticipantState state)
{
    StateCallback cb;
    bool changed = false;

    {
        std::lock_guard<std::mutex> lock(mutex_);

        if (state_ == state) 
        {
            return;
        }

        state_ = state;
        changed = true;
        cb = on_state_changed_;
    }

    if (changed && cb) 
    {
        cb(shared_from_this());
    }
}

ParticipantState Participant::State() const
{
    std::lock_guard<std::mutex> lock(mutex_);
    return state_;
}

bool Participant::IsDisconnected() const
{
    std::lock_guard<std::mutex> lock(mutex_);
    return state_ == ParticipantState::Disconnected;
}

bool Participant::IsActive() const
{
    std::lock_guard<std::mutex> lock(mutex_);
    return state_ == ParticipantState::Active;
}

bool Participant::AddPublishedTrack(const media::MediaTrackPtr& track)
{
    if (!track) {
        return false;
    }

    TrackCallback cb;

    {
        std::lock_guard<std::mutex> lock(mutex_);

        const std::string track_id = track->id();
        if (track_id.empty()) 
        {
            return false;
        }

        if (published_tracks_.find(track_id) != published_tracks_.end()) 
        {
            return false;
        }

        published_tracks_[track_id] = track;
        cb = on_track_published_;
    }

    if (cb) 
    {
        cb(shared_from_this(), track);
    }

    return true;
}

bool Participant::RemovePublishedTrack(const std::string& track_id)
{
    media::MediaTrackPtr removed_track;
    TrackCallback cb;

    {
        std::lock_guard<std::mutex> lock(mutex_);

        auto it = published_tracks_.find(track_id);
        if (it == published_tracks_.end()) 
        {
            return false;
        }

        removed_track = it->second;
        published_tracks_.erase(it);
        cb = on_track_unpublished_;
    }

    if (cb && removed_track) 
    {
        cb(shared_from_this(), removed_track);
    }

    return true;
}

media::MediaTrackPtr Participant::GetPublishedTrack(const std::string& track_id) const
{
    std::lock_guard<std::mutex> lock(mutex_);

    auto it = published_tracks_.find(track_id);
    if (it == published_tracks_.end()) 
    {
        return nullptr;
    }

    return it->second;
}

std::vector<media::MediaTrackPtr> Participant::GetPublishedTracks() const
{
    std::lock_guard<std::mutex> lock(mutex_);

    std::vector<media::MediaTrackPtr> result;
    result.reserve(published_tracks_.size());

    for (const auto& item : published_tracks_) 
    {
        result.push_back(item.second);
    }

    return result;
}

bool Participant::SubscribeTrack(const std::string& track_id)
{
    if (track_id.empty()) 
    {
        return false;
    }

    std::lock_guard<std::mutex> lock(mutex_);

    auto ret = subscribed_track_ids_.insert(track_id);
    return ret.second;
}

bool Participant::SubscribeTrack(Participant::Ptr subscriber, const std::string& track_id)
{
    if (!subscriber)
    {
        return false;
    }

    if (!subscriber->SubscribeTrack(track_id))
    {
        return false;
    }
/*
    auto source_track = FindPublishedTrack(track_id);
    if (!source_track)
    {
        return false;
    }

     // 3. 找订阅者自己的发送通道
    auto session = subscriber->GetSession();
    if (!session)
    {
        return false;
    }

    auto packet_sender = session->GetPacketSender();

    // 4. 创建下游发送轨道
    auto sender_track = RtpSenderTrackFactory::Create(source_track->getTrackInfo(), packet_sender);

    sfu_endpoint_->AddSubscriber(source_track->ssrc(), sender_track);
*/
    return true;
}

bool Participant::UnsubscribeTrack(const std::string& track_id)
{
    std::lock_guard<std::mutex> lock(mutex_);
    return subscribed_track_ids_.erase(track_id) > 0;
}

bool Participant::HasSubscribedTrack(const std::string& track_id) const
{
    std::lock_guard<std::mutex> lock(mutex_);
    return subscribed_track_ids_.find(track_id) != subscribed_track_ids_.end();
}

std::vector<std::string> Participant::GetSubscribedTrackIds() const
{
    std::lock_guard<std::mutex> lock(mutex_);

    std::vector<std::string> result;
    result.reserve(subscribed_track_ids_.size());

    for (const auto& track_id : subscribed_track_ids_) 
    {
        result.push_back(track_id);
    }

    return result;
}

void Participant::Leave()
{
    LeaveCallback cb;

    {
        std::lock_guard<std::mutex> lock(mutex_);

        if (state_ == ParticipantState::Disconnected) 
        {
            return;
        }

        state_ = ParticipantState::Disconnected;
        cb = on_leave_;
    }

    if (cb) 
    {
        cb(shared_from_this());
    }
}

void Participant::OnTrackPublished(TrackCallback cb)
{
    std::lock_guard<std::mutex> lock(mutex_);
    on_track_published_ = std::move(cb);
}

void Participant::OnTrackUnpublished(TrackCallback cb)
{
    std::lock_guard<std::mutex> lock(mutex_);
    on_track_unpublished_ = std::move(cb);
}

void Participant::OnStateChanged(StateCallback cb)
{
    std::lock_guard<std::mutex> lock(mutex_);
    on_state_changed_ = std::move(cb);
}

void Participant::OnLeave(LeaveCallback cb)
{
    std::lock_guard<std::mutex> lock(mutex_);
    on_leave_ = std::move(cb);
}

Participant::TrackCallback Participant::GetOnTrackPublished() const
{
    std::lock_guard<std::mutex> lock(mutex_);
    return on_track_published_;
}

Participant::TrackCallback Participant::GetOnTrackUnpublished() const
{
    std::lock_guard<std::mutex> lock(mutex_);
    return on_track_unpublished_;
}

Participant::StateCallback Participant::GetOnStateChanged() const
{
    std::lock_guard<std::mutex> lock(mutex_);
    return on_state_changed_;
}

Participant::LeaveCallback Participant::GetOnLeave() const
{
    std::lock_guard<std::mutex> lock(mutex_);
    return on_leave_;
}

} // namespace room