#ifndef PACKETIA_MEDIA_STREAM_H_
#define PACKETIA_MEDIA_STREAM_H_

#include "MediaStreamTrack.h"
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

namespace media
{
class IEncodedFramePublisher;
class MediaStreamRegistry;

class MediaStream
{
public:
    using Ptr = std::shared_ptr<MediaStream>;
    enum class State { Announced, Publishing, Closed };

    explicit MediaStream(std::string name);
    uint32_t GetId() const noexcept { return id_; }
    const std::string& GetName() const noexcept { return name_; }
    State GetState() const;
    bool ReplaceTracks(std::vector<MediaStreamTrack> tracks, std::string* error = nullptr);
    std::vector<MediaStreamTrack> GetTracks() const;
    bool GetTrack(TrackId id, MediaStreamTrack* out) const;
    bool BindTrackEndpoint(TrackId track, uint64_t endpoint);
    void UnbindTrackEndpoint(uint64_t endpoint);
    uint64_t FindEndpointByTrack(TrackId track) const;
    bool StartPublishing();
    void SetFramePublisher(std::shared_ptr<IEncodedFramePublisher> publisher);
    std::shared_ptr<IEncodedFramePublisher> GetFramePublisher() const;

private:
    friend class MediaStreamRegistry;
    void Close();
    const uint32_t id_;
    const std::string name_;
    mutable std::mutex mutex_;
    State state_ = State::Announced;
    std::vector<MediaStreamTrack> tracks_;
    std::unordered_map<TrackId, uint64_t> endpoints_;
    std::shared_ptr<IEncodedFramePublisher> publisher_;
    const MediaStreamRegistry* registry_ = nullptr;
};
}

#endif
