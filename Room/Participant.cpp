#include "Participant.h"
#include "media/endpoint/MediaEndpoint.h"

#include <limits>
#include <utility>

namespace room
{

Participant::Participant(std::string participant_id, std::string name)
    : Participant(ParticipantIdentity{std::move(participant_id), std::move(name), {}})
{
}

Participant::Participant(ParticipantIdentity identity)
    : identity_(std::move(identity))
{
}

Participant::~Participant() = default;

ParticipantInfo Participant::GetInfo() const
{
    std::lock_guard<std::mutex> lock(mutex_);
    return {identity_, state_, signaling_, media_session_ ? media_session_->id : 0,
            published_tracks_.size(), subscribed_track_ids_.size()};
}

bool Participant::BindSignaling(std::string connection_id)
{
    std::lock_guard<std::mutex> lock(mutex_);
    if (connection_id.empty() || state_ == ParticipantState::Disconnected ||
        signaling_.generation == std::numeric_limits<uint64_t>::max()) return false;
    signaling_.connection_id = std::move(connection_id);
    ++signaling_.generation;
    return true;
}

bool Participant::UnbindSignaling(const SignalingBinding& expected)
{
    std::lock_guard<std::mutex> lock(mutex_);
    if (expected.connection_id.empty() || expected.connection_id != signaling_.connection_id ||
        expected.generation != signaling_.generation) return false;
    signaling_.connection_id.clear();
    return true;
}

SignalingBinding Participant::GetSignaling() const
{
    std::lock_guard<std::mutex> lock(mutex_);
    return signaling_;
}

bool Participant::BindMediaSession(ParticipantSession::Ptr session)
{
    if (!session || !session->id || !session->endpoint || !session->endpoint->IsRunning()) return false;
    std::lock_guard<std::mutex> lock(mutex_);
    if (state_ == ParticipantState::Disconnected || (media_session_ && media_session_ != session) ||
        (endpoint_ && endpoint_ != session->endpoint)) return false;
    endpoint_ = session->endpoint;
    media_session_ = std::move(session);
    return true;
}

ParticipantSession::Ptr Participant::GetMediaSession() const
{
    std::lock_guard<std::mutex> lock(mutex_);
    return media_session_;
}

bool Participant::BindEndpoint(std::shared_ptr<media::SfuEndpoint> endpoint)
{
    std::lock_guard<std::mutex> lock(mutex_);
    if (!endpoint || !endpoint->IsRunning() || state_ == ParticipantState::Disconnected ||
        (endpoint_ && endpoint_ != endpoint)) return false;
    endpoint_ = std::move(endpoint);
    return true;
}

std::shared_ptr<media::SfuEndpoint> Participant::GetEndpoint() const
{
    std::lock_guard<std::mutex> lock(mutex_);
    return endpoint_;
}

std::string Participant::Id() const
{
    std::lock_guard<std::mutex> lock(mutex_);
    return identity_.participant_id;
}

std::string Participant::Name() const
{
    std::lock_guard<std::mutex> lock(mutex_);
    return identity_.name;
}

void Participant::SetName(const std::string& name)
{
    std::lock_guard<std::mutex> lock(mutex_);
    identity_.name = name;
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

bool Participant::AddPublishedTrack(const media::MediaTrackPtr& track, bool notify)
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

    if (cb && notify)
    {
        cb(shared_from_this(), track);
    }

    return true;
}

void Participant::NotifyTrackPublished(const media::MediaTrackPtr& track)
{
    if (auto callback = GetOnTrackPublished()) callback(shared_from_this(), track);
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
    std::shared_ptr<media::SfuEndpoint> endpoint;
    std::shared_ptr<MediaSession> legacy_session;
    ParticipantSession::Ptr media_session;

    {
        std::lock_guard<std::mutex> lock(mutex_);

        if (state_ == ParticipantState::Disconnected) 
        {
            return;
        }

        state_ = ParticipantState::Disconnected;
        signaling_.connection_id.clear();
        endpoint = std::move(endpoint_);
        published_tracks_.clear();
        subscribed_track_ids_.clear();
        legacy_session = std::move(session_);
        media_session = std::move(media_session_);
        cb = on_leave_;
    }

    if (endpoint) endpoint->Stop();
    legacy_session.reset();
    media_session.reset();
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
