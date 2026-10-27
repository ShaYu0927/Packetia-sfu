#ifndef PACKETIA_MEDIA_TRACK_H
#define PACKETIA_MEDIA_TRACK_H

#include "MediaTrackFwd.h"
#include "TrackMetadata.h"

#include <mutex>
#include <utility>

namespace media
{
/**
 * @brief Thread-safe storage and updates for a track's metadata.
 *
 * MediaTrack owns a TrackMetadata value. It does not process RTP or depend on
 * room membership, signaling, transports, or SFU endpoints.
 */
class MediaTrack
{
public:
    explicit MediaTrack(TrackMetadata info)
        : info_(std::move(info))
    {
    }

    std::string id() const
    {
        std::lock_guard<std::mutex> lock(mutex_);
        return info_.sid;
    }

    MediaType type() const
    {
        std::lock_guard<std::mutex> lock(mutex_);
        return info_.type;
    }

    TrackSource source() const
    {
        std::lock_guard<std::mutex> lock(mutex_);
        return info_.source;
    }

    std::string mimeType() const
    {
        std::lock_guard<std::mutex> lock(mutex_);
        return info_.mimeType;
    }

    bool muted() const
    {
        std::lock_guard<std::mutex> lock(mutex_);
        return info_.muted;
    }

    void setMuted(bool muted)
    {
        std::lock_guard<std::mutex> lock(mutex_);
        info_.muted = muted;
        ++info_.version.version;
    }

    TrackMetadata info() const
    {
        std::lock_guard<std::mutex> lock(mutex_);
        return info_;
    }

    void updateInfo(const TrackMetadata& info)
    {
        std::lock_guard<std::mutex> lock(mutex_);
        info_ = info;
    }

private:
    mutable std::mutex mutex_;
    TrackMetadata info_;
};
}

#endif // PACKETIA_MEDIA_TRACK_H
