#include "Participant.h"
#include "MediaEndpoint.h"
#include "MediaTrack.h"

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
    ParticipantInfo info;
    info.identity = identity_;
    info.state = state_;
    info.signaling = signaling_;
    info.publish_session_id = publish_session_ ? publish_session_->Id() : 0;
    info.subscribe_session_id = subscribe_session_ ? subscribe_session_->Id() : 0;
    // The legacy field describes an unambiguous single-session participant.
    if (!publish_session_ || !subscribe_session_)
        info.media_session_id = info.publish_session_id ? info.publish_session_id : info.subscribe_session_id;
    info.published_track_count = published_tracks_.size();
    info.subscribed_track_count = subscribed_track_ids_.size();
    return info;
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
    if (!session) return false;
    const auto role = session->Role();
    return BindRoleSession(std::move(session), role);
}

ParticipantSession::Ptr Participant::GetMediaSession() const
{
    std::lock_guard<std::mutex> lock(mutex_);
    if (publish_session_ && subscribe_session_) return {};
    return publish_session_ ? publish_session_ : subscribe_session_;
}

bool Participant::BindEndpoint(std::shared_ptr<media::SfuEndpoint> endpoint)
{
    std::lock_guard<std::mutex> lock(mutex_);
    if (!endpoint || !endpoint->IsRunning() || state_ == ParticipantState::Disconnected ||
        (legacy_endpoint_ && legacy_endpoint_ != endpoint) ||
        (publish_session_ && publish_session_->Endpoint() != endpoint) ||
        (subscribe_session_ && subscribe_session_->Endpoint() != endpoint)) return false;
    legacy_endpoint_ = std::move(endpoint);
    return true;
}

std::shared_ptr<media::SfuEndpoint> Participant::GetEndpoint() const
{
    std::lock_guard<std::mutex> lock(mutex_);
    if (legacy_endpoint_) return legacy_endpoint_;
    const auto published = publish_session_ ? publish_session_->Endpoint() : nullptr;
    const auto subscribed = subscribe_session_ ? subscribe_session_->Endpoint() : nullptr;
    if (published && subscribed && published != subscribed) return {};
    return published ? published : subscribed;
}

std::shared_ptr<media::SfuEndpoint> Participant::GetPublishEndpoint() const
{
    std::lock_guard<std::mutex> lock(mutex_);
    return publish_session_ ? publish_session_->Endpoint() : legacy_endpoint_;
}

std::shared_ptr<media::SfuEndpoint> Participant::GetSubscribeEndpoint() const
{
    std::lock_guard<std::mutex> lock(mutex_);
    return subscribe_session_ ? subscribe_session_->Endpoint() : legacy_endpoint_;
}

bool Participant::HasMediaBinding() const
{
    std::lock_guard<std::mutex> lock(mutex_);
    return legacy_endpoint_ || publish_session_ || subscribe_session_;
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
    std::shared_ptr<media::SfuEndpoint> legacy_endpoint;
    ParticipantSession::Ptr publication, subscription;

    {
        std::lock_guard<std::mutex> lock(mutex_);

        if (state_ == ParticipantState::Disconnected) 
        {
            return;
        }

        state_ = ParticipantState::Disconnected;
        signaling_.connection_id.clear();
        legacy_endpoint = std::move(legacy_endpoint_);
        published_tracks_.clear();
        subscribed_track_ids_.clear();
        publication = std::move(publish_session_);
        subscription = std::move(subscribe_session_);
        cb = on_leave_;
    }

    const auto published = publication ? publication->Endpoint() : nullptr;
    const auto subscribed = subscription ? subscription->Endpoint() : nullptr;
    if (published) published->Stop();
    if (subscribed && subscribed != published) subscribed->Stop();
    if (legacy_endpoint && legacy_endpoint != published && legacy_endpoint != subscribed)
        legacy_endpoint->Stop();
    publication.reset();
    subscription.reset();
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

bool Participant::BindPublishSession(ParticipantSession::Ptr session)
{
    return BindRoleSession(std::move(session), ParticipantSessionRole::Publish);
}

bool Participant::BindSubscribeSession(ParticipantSession::Ptr session)
{
    return BindRoleSession(std::move(session), ParticipantSessionRole::Subscribe);
}

bool Participant::BindRoleSession(ParticipantSession::Ptr session, ParticipantSessionRole role)
{
    if (!session || session->Role() != role || !session->Id() ||
        !session->Endpoint() || !session->Endpoint()->IsRunning()) return false;
    if (role != ParticipantSessionRole::Publish && role != ParticipantSessionRole::Subscribe) return false;
    std::lock_guard<std::mutex> lock(mutex_);
    auto& current = role == ParticipantSessionRole::Publish ? publish_session_ : subscribe_session_;
    const auto& other = role == ParticipantSessionRole::Publish ? subscribe_session_ : publish_session_;
    if (state_ == ParticipantState::Disconnected || (current && current != session) ||
        (other && other->Id() == session->Id()) ||
        (legacy_endpoint_ && legacy_endpoint_ != session->Endpoint())) return false;
    current = std::move(session);
    return true;
}

ParticipantSession::Ptr Participant::GetPublishSession() const
{
    std::lock_guard<std::mutex> lock(mutex_);
    return publish_session_;
}

ParticipantSession::Ptr Participant::GetSubscribeSession() const
{
    std::lock_guard<std::mutex> lock(mutex_);
    return subscribe_session_;
}

} // namespace room
