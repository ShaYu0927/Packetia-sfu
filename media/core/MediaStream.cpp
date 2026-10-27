#include "MediaStream.h"
#include <algorithm>
#include <atomic>
#include <limits>
#include <stdexcept>
#include <unordered_set>

namespace media
{
namespace
{
uint32_t NextStreamId()
{
    static std::atomic<uint64_t> next{0};
    const auto id = ++next;
    if (id > std::numeric_limits<uint32_t>::max())
        throw std::overflow_error("media stream ID exhausted");
    return static_cast<uint32_t>(id);
}
}

MediaStream::MediaStream(std::string name) : id_(NextStreamId()), name_(std::move(name)) {}

MediaStream::State MediaStream::GetState() const
{
    std::lock_guard<std::mutex> lock(mutex_);
    return state_;
}

bool MediaStream::ReplaceTracks(std::vector<MediaStreamTrack> tracks, std::string* error)
{
    const auto fail = [&](const char* message) {
        if (error) *error = message;
        return false;
    };
    if (tracks.empty()) return fail("empty media stream");
    std::unordered_set<TrackId> ids;
    for (const auto& track : tracks)
    {
        if (!ids.insert(track.id).second) return fail("duplicate media track ID");
        if (!track.Valid())
            return fail("invalid media track parameters");
    }
    std::lock_guard<std::mutex> lock(mutex_);
    if (state_ == State::Closed || !endpoints_.empty()) return fail("media stream is closed or bound");
    tracks_ = std::move(tracks);
    if (error) error->clear();
    return true;
}

std::vector<MediaStreamTrack> MediaStream::GetTracks() const
{
    std::lock_guard<std::mutex> lock(mutex_);
    return tracks_;
}

bool MediaStream::GetTrack(TrackId id, MediaStreamTrack* out) const
{
    if (!out) return false;
    std::lock_guard<std::mutex> lock(mutex_);
    for (const auto& track : tracks_)
        if (track.id == id) { *out = track; return true; }
    return false;
}

bool MediaStream::BindTrackEndpoint(TrackId track, uint64_t endpoint)
{
    std::lock_guard<std::mutex> lock(mutex_);
    if (state_ == State::Closed || endpoint == 0 || endpoints_.count(track)) return false;
    if (std::none_of(tracks_.begin(), tracks_.end(),
        [track](const auto& item) { return item.id == track; })) return false;
    endpoints_[track] = endpoint;
    return true;
}

void MediaStream::UnbindTrackEndpoint(uint64_t endpoint)
{
    std::lock_guard<std::mutex> lock(mutex_);
    for (auto it = endpoints_.begin(); it != endpoints_.end();)
        if (it->second == endpoint) it = endpoints_.erase(it);
        else ++it;
    if (endpoints_.empty() && state_ == State::Publishing) state_ = State::Announced;
}

uint64_t MediaStream::FindEndpointByTrack(TrackId track) const
{
    std::lock_guard<std::mutex> lock(mutex_);
    const auto it = endpoints_.find(track);
    return it == endpoints_.end() ? 0 : it->second;
}

bool MediaStream::StartPublishing()
{
    std::lock_guard<std::mutex> lock(mutex_);
    if (state_ == State::Closed || endpoints_.empty()) return false;
    state_ = State::Publishing;
    return true;
}

void MediaStream::SetFramePublisher(std::shared_ptr<IEncodedFramePublisher> publisher)
{
    std::lock_guard<std::mutex> lock(mutex_);
    if (state_ != State::Closed) publisher_ = std::move(publisher);
}

std::shared_ptr<IEncodedFramePublisher> MediaStream::GetFramePublisher() const
{
    std::lock_guard<std::mutex> lock(mutex_);
    return publisher_;
}

void MediaStream::Close()
{
    std::lock_guard<std::mutex> lock(mutex_);
    state_ = State::Closed;
    endpoints_.clear();
    publisher_.reset();
    registry_ = nullptr;
}
}
