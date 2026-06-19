#include "Room.h"

#include <algorithm>
#include <chrono>
#include <utility>

namespace room
{
namespace
{
uint64_t NowMs()
{
    using namespace std::chrono;
    return duration_cast<milliseconds>(steady_clock::now().time_since_epoch()).count();
}
}

Room::Room(RoomInfo info, RoomOptions options)
    : info_(std::move(info)),
      options_(options)
{
    if (info_.created_at_ms == 0)
    {
        info_.created_at_ms = NowMs();
    }
}

Room::~Room() = default;

const std::string& Room::Id() const
{
    return info_.room_id;
}

const std::string& Room::Name() const
{
    return info_.room_name;
}

RoomInfo Room::GetInfo() const
{
    std::lock_guard<std::mutex> lock(mutex_);
    return info_;
}

RoomState Room::State() const
{
    std::lock_guard<std::mutex> lock(mutex_);
    return info_.state;
}

bool Room::IsClosed() const
{
    std::lock_guard<std::mutex> lock(mutex_);
    return info_.state == RoomState::Closed;
}

bool Room::Join(const Participant::Ptr& participant)
{
    if (!participant)
    {
        return false;
    }

    std::vector<std::string> auto_subscribe_tracks;
    std::function<void(Participant::Ptr)> cb;

    {
        std::lock_guard<std::mutex> lock(mutex_);

        if (!CanJoinLocked(participant))
        {
            return false;
        }

        const std::string participant_id = participant->Id();
        participants_[participant_id] = participant;

        if (info_.first_joined_at_ms == 0)
        {
            info_.first_joined_at_ms = NowMs();
        }

        if (options_.auto_subscribe)
        {
            auto_subscribe_tracks.reserve(published_tracks_.size());
            for (const auto& item : published_tracks_)
            {
                if (item.second.publisher_id != participant_id)
                {
                    auto_subscribe_tracks.push_back(item.first);
                }
            }
        }

        for (const auto& track_id : auto_subscribe_tracks)
        {
            SubscribeTrackLocked(participant_id, track_id);
        }

        UpdateStateLocked();
        cb = on_participant_changed_;
    }

    participant->SetState(ParticipantState::Joined);
    if (cb)
    {
        cb(participant);
    }

    return true;
}

bool Room::Leave(const std::string& participant_id)
{
    Participant::Ptr participant;
    std::vector<std::string> published_track_ids;
    std::function<void(Participant::Ptr)> cb;

    {
        std::lock_guard<std::mutex> lock(mutex_);

        auto it = participants_.find(participant_id);
        if (it == participants_.end())
        {
            return false;
        }

        participant = it->second;

        auto sub_it = participant_subscriptions_.find(participant_id);
        if (sub_it != participant_subscriptions_.end())
        {
            std::vector<std::string> track_ids(sub_it->second.begin(), sub_it->second.end());
            for (const auto& track_id : track_ids)
            {
                UnsubscribeTrackLocked(participant_id, track_id);
            }
        }

        for (const auto& item : published_tracks_)
        {
            if (item.second.publisher_id == participant_id)
            {
                published_track_ids.push_back(item.first);
            }
        }

        for (const auto& track_id : published_track_ids)
        {
            published_tracks_.erase(track_id);
            track_subscribers_.erase(track_id);

            for (auto& sub_item : participant_subscriptions_)
            {
                sub_item.second.erase(track_id);
            }
        }

        participants_.erase(it);
        info_.last_left_at_ms = NowMs();
        UpdateStateLocked();
        cb = on_participant_changed_;
    }

    if (participant)
    {
        participant->Leave();
        if (cb)
        {
            cb(participant);
        }
    }

    return true;
}

Participant::Ptr Room::GetParticipant(const std::string& participant_id) const
{
    std::lock_guard<std::mutex> lock(mutex_);

    auto it = participants_.find(participant_id);
    if (it == participants_.end())
    {
        return nullptr;
    }

    return it->second;
}

std::vector<Participant::Ptr> Room::GetParticipants() const
{
    std::lock_guard<std::mutex> lock(mutex_);

    std::vector<Participant::Ptr> result;
    result.reserve(participants_.size());
    for (const auto& item : participants_)
    {
        result.push_back(item.second);
    }

    return result;
}

size_t Room::ParticipantCount() const
{
    std::lock_guard<std::mutex> lock(mutex_);
    return participants_.size();
}

bool Room::PublishTrack(const std::string& participant_id,
                        const media::MediaTrackPtr& track,
                        uint32_t ssrc,
                        uint8_t payload_type)
{
    if (!track || track->id().empty())
    {
        return false;
    }

    std::vector<std::string> auto_subscribers;

    {
        std::lock_guard<std::mutex> lock(mutex_);

        auto participant_it = participants_.find(participant_id);
        if (participant_it == participants_.end())
        {
            return false;
        }

        const std::string track_id = track->id();
        if (published_tracks_.find(track_id) != published_tracks_.end())
        {
            return false;
        }

        if (!participant_it->second->AddPublishedTrack(track))
        {
            return false;
        }

        PublishedTrackInfo info;
        info.publisher_id = participant_id;
        info.track = track;
        info.ssrc = ssrc;
        info.payload_type = payload_type;
        published_tracks_[track_id] = std::move(info);

        if (options_.auto_subscribe)
        {
            for (const auto& item : participants_)
            {
                if (item.first != participant_id)
                {
                    auto_subscribers.push_back(item.first);
                }
            }
        }

        for (const auto& subscriber_id : auto_subscribers)
        {
            SubscribeTrackLocked(subscriber_id, track_id);
        }
    }

    return true;
}

bool Room::UnpublishTrack(const std::string& track_id)
{
    if (track_id.empty())
    {
        return false;
    }

    Participant::Ptr publisher;

    {
        std::lock_guard<std::mutex> lock(mutex_);

        auto track_it = published_tracks_.find(track_id);
        if (track_it == published_tracks_.end())
        {
            return false;
        }

        auto publisher_it = participants_.find(track_it->second.publisher_id);
        if (publisher_it != participants_.end())
        {
            publisher = publisher_it->second;
        }

        auto subscribers_it = track_subscribers_.find(track_id);
        if (subscribers_it != track_subscribers_.end())
        {
            std::vector<std::string> subscriber_ids(subscribers_it->second.begin(),
                                                    subscribers_it->second.end());
            for (const auto& subscriber_id : subscriber_ids)
            {
                UnsubscribeTrackLocked(subscriber_id, track_id);
            }
        }

        published_tracks_.erase(track_it);
    }

    if (publisher)
    {
        publisher->RemovePublishedTrack(track_id);
    }

    return true;
}

bool Room::SubscribeTrack(const std::string& subscriber_id,
                          const std::string& track_id)
{
    std::lock_guard<std::mutex> lock(mutex_);
    return SubscribeTrackLocked(subscriber_id, track_id);
}

bool Room::UnsubscribeTrack(const std::string& subscriber_id,
                            const std::string& track_id)
{
    std::lock_guard<std::mutex> lock(mutex_);

    const bool subscribed =
        participant_subscriptions_.find(subscriber_id) != participant_subscriptions_.end() &&
        participant_subscriptions_[subscriber_id].find(track_id) != participant_subscriptions_[subscriber_id].end();

    UnsubscribeTrackLocked(subscriber_id, track_id);
    return subscribed;
}

std::vector<SubscriptionInfo> Room::GetSubscriptionsByTrack(const std::string& track_id) const
{
    std::lock_guard<std::mutex> lock(mutex_);

    std::vector<SubscriptionInfo> result;
    auto subs_it = track_subscribers_.find(track_id);
    auto track_it = published_tracks_.find(track_id);
    if (subs_it == track_subscribers_.end() || track_it == published_tracks_.end())
    {
        return result;
    }

    result.reserve(subs_it->second.size());
    for (const auto& subscriber_id : subs_it->second)
    {
        result.push_back({subscriber_id, track_it->second.publisher_id, track_id});
    }

    return result;
}

std::vector<SubscriptionInfo> Room::GetSubscriptionsBySubscriber(const std::string& subscriber_id) const
{
    std::lock_guard<std::mutex> lock(mutex_);

    std::vector<SubscriptionInfo> result;
    auto subs_it = participant_subscriptions_.find(subscriber_id);
    if (subs_it == participant_subscriptions_.end())
    {
        return result;
    }

    result.reserve(subs_it->second.size());
    for (const auto& track_id : subs_it->second)
    {
        auto track_it = published_tracks_.find(track_id);
        if (track_it == published_tracks_.end())
        {
            continue;
        }

        result.push_back({subscriber_id, track_it->second.publisher_id, track_id});
    }

    return result;
}

std::vector<Participant::Ptr> Room::GetSubscribers(const std::string& track_id) const
{
    std::lock_guard<std::mutex> lock(mutex_);

    std::vector<Participant::Ptr> result;
    auto subs_it = track_subscribers_.find(track_id);
    if (subs_it == track_subscribers_.end())
    {
        return result;
    }

    result.reserve(subs_it->second.size());
    for (const auto& subscriber_id : subs_it->second)
    {
        auto participant_it = participants_.find(subscriber_id);
        if (participant_it != participants_.end())
        {
            result.push_back(participant_it->second);
        }
    }

    return result;
}

media::MediaTrackPtr Room::ResolveTrackForSubscriber(const std::string& subscriber_id,
                                                     const std::string& track_id)
{
    std::lock_guard<std::mutex> lock(mutex_);

    auto participant_it = participant_subscriptions_.find(subscriber_id);
    if (participant_it == participant_subscriptions_.end() ||
        participant_it->second.find(track_id) == participant_it->second.end())
    {
        return nullptr;
    }

    auto track_it = published_tracks_.find(track_id);
    if (track_it == published_tracks_.end())
    {
        return nullptr;
    }

    return track_it->second.track;
}

void Room::Close()
{
    std::function<void(const std::string&)> cb;

    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (info_.state == RoomState::Closed)
        {
            return;
        }

        info_.state = RoomState::Closed;
        participants_.clear();
        published_tracks_.clear();
        track_subscribers_.clear();
        participant_subscriptions_.clear();
        info_.participant_count = 0;
        cb = on_room_closed_;
    }

    if (cb)
    {
        cb(info_.room_id);
    }
}

void Room::SetOnParticipantChanged(std::function<void(Participant::Ptr)> cb)
{
    std::lock_guard<std::mutex> lock(mutex_);
    on_participant_changed_ = std::move(cb);
}

void Room::SetOnRoomClosed(std::function<void(const std::string& room_id)> cb)
{
    std::lock_guard<std::mutex> lock(mutex_);
    on_room_closed_ = std::move(cb);
}

bool Room::CanJoinLocked(const Participant::Ptr& participant) const
{
    if (!participant || info_.state == RoomState::Closed)
    {
        return false;
    }

    const std::string participant_id = participant->Id();
    if (participant_id.empty() || participants_.find(participant_id) != participants_.end())
    {
        return false;
    }

    if (options_.max_participants > 0 &&
        participants_.size() >= options_.max_participants)
    {
        return false;
    }

    return true;
}

void Room::UpdateStateLocked()
{
    info_.participant_count = static_cast<uint32_t>(participants_.size());

    if (info_.state == RoomState::Closed)
    {
        return;
    }

    info_.state = participants_.empty() ? RoomState::Empty : RoomState::Active;
}

bool Room::SubscribeTrackLocked(const std::string& subscriber_id,
                                const std::string& track_id)
{
    if (subscriber_id.empty() || track_id.empty())
    {
        return false;
    }

    auto subscriber_it = participants_.find(subscriber_id);
    auto track_it = published_tracks_.find(track_id);
    if (subscriber_it == participants_.end() || track_it == published_tracks_.end())
    {
        return false;
    }

    if (track_it->second.publisher_id == subscriber_id)
    {
        return false;
    }

    auto& track_subscribers = track_subscribers_[track_id];
    auto inserted = track_subscribers.insert(subscriber_id);
    if (!inserted.second)
    {
        return false;
    }

    participant_subscriptions_[subscriber_id].insert(track_id);
    subscriber_it->second->SubscribeTrack(track_id);
    return true;
}

void Room::UnsubscribeTrackLocked(const std::string& subscriber_id,
                                  const std::string& track_id)
{
    auto track_it = track_subscribers_.find(track_id);
    if (track_it != track_subscribers_.end())
    {
        track_it->second.erase(subscriber_id);
        if (track_it->second.empty())
        {
            track_subscribers_.erase(track_it);
        }
    }

    auto participant_it = participant_subscriptions_.find(subscriber_id);
    if (participant_it != participant_subscriptions_.end())
    {
        participant_it->second.erase(track_id);
        if (participant_it->second.empty())
        {
            participant_subscriptions_.erase(participant_it);
        }
    }

    auto subscriber_it = participants_.find(subscriber_id);
    if (subscriber_it != participants_.end())
    {
        subscriber_it->second->UnsubscribeTrack(track_id);
    }
}

}
