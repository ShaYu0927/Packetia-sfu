#ifndef PACKETIA_MEDIA_STREAM_REGISTRY_H_
#define PACKETIA_MEDIA_STREAM_REGISTRY_H_

#include "MediaStream.h"
#include "EventBus.h"
#include "MediaTrackSubscription.h"
#include <optional>

namespace media
{

class MediaStreamRegistry
{
public:
    struct ConsumerSnapshot
    {
        uint64_t id = 0;
        MediaTrackRef track;
        std::string name;
        bool paused = false;
    };
    struct TrackSnapshot : MediaStream::TrackSnapshot
    {
        MediaTrackState state = MediaTrackState::Open;
        std::vector<ConsumerSnapshot> consumers;
    };
    struct Snapshot
    {
        uint64_t revision = 0;
        std::vector<MediaStream::Snapshot> streams;
        std::vector<TrackSnapshot> tracks;
    };
    ~MediaStreamRegistry();
    static MediaStreamRegistry& Instance();
    bool Register(const MediaStream::Ptr& stream);
    MediaStream::Ptr FindById(uint32_t id) const;
    MediaStream::Ptr FindByName(const std::string& name, const std::string& scope = {}) const;
    Snapshot GetSnapshot() const;
    std::optional<TrackSnapshot> FindTrack(MediaTrackRef ref) const;
    MediaTrackSubscription::Ptr AttachConsumer(MediaTrackRef ref, std::string name,std::function<void()> on_close = {});
    bool SetTrackPaused(MediaTrackRef ref, bool paused);
    bool SetConsumerPaused(uint64_t id, bool paused);
    bool DetachConsumer(uint64_t id);
    bool RemoveTrack(MediaTrackRef ref);
    EventBus& Events() { return events_; }
    bool Remove(const MediaStream::Ptr& expected);
    std::size_t Size() const;
    void SetFramePublisher(std::shared_ptr<IEncodedFramePublisher> publisher);
    void Clear();

private:
    struct TrackEntry
    {
        std::shared_ptr<std::atomic<MediaTrackState>> state = std::make_shared<std::atomic<MediaTrackState>>(MediaTrackState::Open);
        std::unordered_map<uint64_t, std::weak_ptr<MediaTrackSubscription>> consumers;
    };
    static uint64_t TrackKey(MediaTrackRef ref) { return (uint64_t(ref.stream_id) << 32) | ref.track_id; }
    void CloseTracksLocked(const MediaStream::Ptr& stream, std::vector<MediaTrackSubscription::Ptr>& retired);
    void PruneConsumersLocked(TrackEntry& track, std::vector<MediaTrackSubscription::Ptr>& retained);
    TrackSnapshot SnapshotTrackLocked(const MediaStream::TrackSnapshot& track, const TrackEntry& entry, std::vector<MediaTrackSubscription::Ptr>& retained) const;
    mutable std::mutex mutex_;
    std::unordered_map<uint32_t, MediaStream::Ptr> streams_;
    std::unordered_map<std::string, std::unordered_map<std::string, MediaStream::Ptr>> names_;
    std::shared_ptr<IEncodedFramePublisher> publisher_;
    uint64_t revision_ = 0;
    EventBus events_;
    std::unordered_map<uint64_t, TrackEntry> tracks_;
    std::unordered_map<uint64_t, std::weak_ptr<MediaTrackSubscription>> consumers_;
    uint64_t next_consumer_id_ = 1;
};
}

#endif
