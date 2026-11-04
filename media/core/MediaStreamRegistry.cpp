#include "MediaStreamRegistry.h"
#include "MediaStreamEvent.h"
#include "MediaTrackEvent.h"
#include <algorithm>

namespace media
{
MediaStreamRegistry::~MediaStreamRegistry()
{
    // 析构只释放资源，不再调用可能引用服务的业务订阅者。
    for (const auto& entry : tracks_) entry.second.state->store(MediaTrackState::Closed);
    for (const auto& entry : streams_) entry.second->Close();
}

MediaStreamRegistry& MediaStreamRegistry::Instance()
{
    static MediaStreamRegistry registry;
    return registry;
}

bool MediaStreamRegistry::Register(const MediaStream::Ptr& stream)
{
    if (!stream || stream->GetName().empty()) return false;
    MediaStream::Snapshot snapshot;
    uint64_t revision;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        const auto scope = names_.find(stream->GetSource().scope);
        if (scope != names_.end() && scope->second.count(stream->GetName())) return false;
        
        std::lock_guard<std::mutex> stream_lock(stream->mutex_);
        if (streams_.count(stream->GetId()) || stream->registry_ || stream->state_ == MediaStream::State::Closed || stream->tracks_.empty()) return false;
        snapshot = {stream->id_, stream->name_, stream->source_, stream->state_, {}};
        std::unordered_map<uint64_t, TrackEntry> additions;
        for (const auto& track : stream->tracks_)
        {
            const auto endpoint = stream->endpoints_.find(track.id);
            snapshot.tracks.push_back({track.ref, track,
                endpoint == stream->endpoints_.end() ? 0 : endpoint->second});
            additions.try_emplace(TrackKey(track.ref));
        }
        tracks_.reserve(tracks_.size() + additions.size());
        try
        {
            streams_.emplace(stream->GetId(), stream);
            names_[stream->GetSource().scope].emplace(stream->GetName(), stream);
            tracks_.merge(additions);
        }
        catch (...)
        {
            streams_.erase(stream->GetId());
            const auto names = names_.find(stream->GetSource().scope);
            if (names != names_.end())
            {
                names->second.erase(stream->GetName());
                if (names->second.empty()) names_.erase(names);
            }
            for (const auto& track : snapshot.tracks) tracks_.erase(TrackKey(track.ref));
            throw;
        }
        stream->publisher_ = publisher_;
        stream->registry_ = this;
        revision = ++revision_;
    }
    events_.EmplacePublish<StreamRegisteredEvent>(std::move(snapshot), revision);
    return true;
}

MediaStream::Ptr MediaStreamRegistry::FindById(uint32_t id) const
{
    std::lock_guard<std::mutex> lock(mutex_);
    const auto it = streams_.find(id);
    return it == streams_.end() ? nullptr : it->second;
}

MediaStream::Ptr MediaStreamRegistry::FindByName(const std::string& name, const std::string& scope) const
{
    std::lock_guard<std::mutex> lock(mutex_);
    const auto found = names_.find(scope);
    if (found == names_.end()) return nullptr;
    const auto it = found->second.find(name);
    return it == found->second.end() ? nullptr : it->second;
}

MediaStreamRegistry::Snapshot MediaStreamRegistry::GetSnapshot() const
{
    Snapshot snapshot;
    // 弱引用提升的最后一个持有者也在锁外释放，避免回调捕获对象析构时重入。
    std::vector<MediaTrackSubscription::Ptr> retained;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        retained.reserve(consumers_.size());
        snapshot.revision = revision_;
        snapshot.streams.reserve(streams_.size());
        for (const auto& entry : streams_)
        {
            auto stream = entry.second->GetSnapshot();
            for (const auto& track : stream.tracks)
            {
                const auto found = tracks_.find(TrackKey(track.ref));
                if (found != tracks_.end()) snapshot.tracks.push_back(SnapshotTrackLocked(track, found->second, retained));
            }
            snapshot.streams.push_back(std::move(stream));
        }
    }
    std::sort(snapshot.streams.begin(), snapshot.streams.end(),
        [](const auto& a, const auto& b) { return a.id < b.id; });
    std::sort(snapshot.tracks.begin(), snapshot.tracks.end(),
        [](const auto& a, const auto& b) { return TrackKey(a.ref) < TrackKey(b.ref); });
    return snapshot;
}

std::optional<MediaStreamRegistry::TrackSnapshot> MediaStreamRegistry::FindTrack(MediaTrackRef ref) const
{
    std::vector<MediaTrackSubscription::Ptr> retained;
    std::lock_guard<std::mutex> lock(mutex_);
    const auto entry = tracks_.find(TrackKey(ref));
    const auto stream = streams_.find(ref.stream_id);
    if (entry == tracks_.end() || stream == streams_.end()) return {};
    retained.reserve(entry->second.consumers.size());
    TrackSnapshot snapshot;
    snapshot.ref = ref;
    if (!stream->second->GetTrack(ref.track_id, &snapshot.description)) return {};
    snapshot.endpoint_id = stream->second->FindEndpointByTrack(ref.track_id);
    return SnapshotTrackLocked(snapshot, entry->second, retained);
}

MediaStreamRegistry::TrackSnapshot MediaStreamRegistry::SnapshotTrackLocked(
    const MediaStream::TrackSnapshot& track, const TrackEntry& entry,
    std::vector<MediaTrackSubscription::Ptr>& retained) const
{
    TrackSnapshot snapshot;
    static_cast<MediaStream::TrackSnapshot&>(snapshot) = track;
    snapshot.state = entry.state->load();
    for (const auto& item : entry.consumers)
        if (const auto consumer = item.second.lock())
        {
            retained.push_back(consumer);
            if (!consumer->Closed())
                snapshot.consumers.push_back({consumer->Id(), track.ref, consumer->Name(), consumer->Paused()});
        }
    std::sort(snapshot.consumers.begin(), snapshot.consumers.end(),
        [](const auto& a, const auto& b) { return a.id < b.id; });
    return snapshot;
}

void MediaStreamRegistry::PruneConsumersLocked(TrackEntry& track, std::vector<MediaTrackSubscription::Ptr>& retained)
{
    retained.reserve(track.consumers.size());
    for (auto it = track.consumers.begin(); it != track.consumers.end();)
    {
        const auto consumer = it->second.lock();
        if (consumer) retained.push_back(consumer);
        if (!consumer || consumer->Closed())
        {
            consumers_.erase(it->first);
            it = track.consumers.erase(it);
        }
        else ++it;
    }
}

MediaTrackSubscription::Ptr MediaStreamRegistry::AttachConsumer(
    MediaTrackRef ref, std::string name, std::function<void()> on_close)
{
    if (!ref.Valid() || name.empty()) return {};
    std::vector<MediaTrackSubscription::Ptr> retained;
    MediaTrackSubscription::Ptr consumer;
    std::lock_guard<std::mutex> lock(mutex_);
    const auto found = tracks_.find(TrackKey(ref));
    const auto stream = streams_.find(ref.stream_id);
    if (found == tracks_.end() || stream == streams_.end() || !next_consumer_id_ ||
        stream->second->GetState() != MediaStream::State::Publishing ||
        !stream->second->FindEndpointByTrack(ref.track_id)) return {};
    auto& entry = found->second;
    PruneConsumersLocked(entry, retained);
    if (entry.consumers.size() >= 4096) return {};
    const auto id = next_consumer_id_++;
    consumer = MediaTrackSubscription::Ptr(new MediaTrackSubscription(
        id, ref, std::move(name), entry.state, std::move(on_close)));
    entry.consumers.emplace(id, consumer);
    consumers_.emplace(id, consumer);
    return consumer;
}

bool MediaStreamRegistry::SetTrackPaused(MediaTrackRef ref, bool paused)
{
    const auto state = paused ? MediaTrackState::Paused : MediaTrackState::Open;
    uint64_t revision;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        const auto found = tracks_.find(TrackKey(ref));
        if (found == tracks_.end()) return false;
        if (found->second.state->exchange(state) == state) return true;
        revision = ++revision_;
    }
    events_.EmplacePublish<MediaTrackStateEvent>(ref, state, revision);
    return true;
}

bool MediaStreamRegistry::SetConsumerPaused(uint64_t id, bool paused)
{
    MediaTrackSubscription::Ptr consumer;
    std::lock_guard<std::mutex> lock(mutex_);
    const auto found = consumers_.find(id);
    if (found == consumers_.end()) return false;
    consumer = found->second.lock();
    if (!consumer || consumer->Closed()) return false;
    consumer->paused_.store(paused);
    return true;
}

bool MediaStreamRegistry::DetachConsumer(uint64_t id)
{
    MediaTrackSubscription::Ptr consumer;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        const auto found = consumers_.find(id);
        if (found == consumers_.end()) return false;
        consumer = found->second.lock();
        consumers_.erase(found);
        if (consumer)
        {
            const auto track = tracks_.find(TrackKey(consumer->Track()));
            if (track != tracks_.end()) track->second.consumers.erase(id);
        }
    }
    return consumer && consumer->NotifyClosed();
}

bool MediaStreamRegistry::RemoveTrack(MediaTrackRef ref)
{
    std::vector<MediaTrackSubscription::Ptr> retired;
    uint64_t revision;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        const auto found = tracks_.find(TrackKey(ref));
        if (found == tracks_.end()) return false;
        auto stream = streams_.at(ref.stream_id);
        found->second.state->store(MediaTrackState::Closed);
        for (const auto& item : found->second.consumers)
        {
            if (auto consumer = item.second.lock()) retired.push_back(std::move(consumer));
            consumers_.erase(item.first);
        }
        tracks_.erase(found);
        {
            std::lock_guard<std::mutex> stream_lock(stream->mutex_);
            auto& tracks = stream->tracks_;
            tracks.erase(std::remove_if(tracks.begin(), tracks.end(),
                [ref](const auto& track) { return track.id == ref.track_id; }), tracks.end());
            stream->endpoints_.erase(ref.track_id);
            if (stream->endpoints_.empty()) stream->state_ = MediaStream::State::Announced;
        }
        revision = ++revision_;
    }
    // 闸门已关闭；清理回调不持有注册表锁，可投递到消费端自己的执行器。
    for (const auto& consumer : retired) consumer->NotifyClosed();
    events_.EmplacePublish<MediaTrackStateEvent>(ref, MediaTrackState::Closed, revision);
    return true;
}

void MediaStreamRegistry::CloseTracksLocked(const MediaStream::Ptr& stream, std::vector<MediaTrackSubscription::Ptr>& retired)
{
    for (const auto& track : stream->GetTracks())
    {
        const auto it = tracks_.find(TrackKey(track.ref));
        if (it == tracks_.end()) continue;
        it->second.state->store(MediaTrackState::Closed);
        for (const auto& item : it->second.consumers)
        {
            if (auto consumer = item.second.lock()) retired.push_back(std::move(consumer));
            consumers_.erase(item.first);
        }
        tracks_.erase(it);
    }
}

bool MediaStreamRegistry::Remove(const MediaStream::Ptr& expected)
{
    if (!expected) return false;
    MediaStream::Snapshot snapshot;
    uint64_t revision;
    std::vector<MediaTrackSubscription::Ptr> retired;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        const auto it = streams_.find(expected->GetId());
        if (it == streams_.end() || it->second != expected) return false;
        auto scope = names_.find(expected->GetSource().scope);
        scope->second.erase(expected->GetName());
        if (scope->second.empty()) names_.erase(scope);
        streams_.erase(it);
        CloseTracksLocked(expected, retired);
        expected->Close();
        snapshot = expected->GetSnapshot();
        revision = ++revision_;
    }
    for (const auto& consumer : retired) consumer->NotifyClosed();
    events_.EmplacePublish<StreamUnregisteredEvent>(std::move(snapshot), revision);
    return true;
}

std::size_t MediaStreamRegistry::Size() const
{
    std::lock_guard<std::mutex> lock(mutex_);
    return streams_.size();
}

void MediaStreamRegistry::SetFramePublisher(std::shared_ptr<IEncodedFramePublisher> publisher)
{
    std::lock_guard<std::mutex> lock(mutex_);
    publisher_ = std::move(publisher);
    for (const auto& entry : streams_) entry.second->SetFramePublisher(publisher_);
}

void MediaStreamRegistry::Clear()
{
    std::vector<std::pair<MediaStream::Snapshot, uint64_t>> removed;
    std::vector<MediaTrackSubscription::Ptr> retired;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        removed.reserve(streams_.size());
        for (const auto& entry : streams_)
        {
            CloseTracksLocked(entry.second, retired);
            entry.second->Close();
            removed.emplace_back(entry.second->GetSnapshot(), ++revision_);
        }
        streams_.clear();
        names_.clear();
    }
    for (const auto& consumer : retired) consumer->NotifyClosed();
    for (auto& entry : removed)
        events_.EmplacePublish<StreamUnregisteredEvent>(std::move(entry.first), entry.second);
}
}
