#ifndef PACKETIA_MEDIA_TRACK_H
#define PACKETIA_MEDIA_TRACK_H

#include "MediaTrackFwd.h"
#include "TrackTypes.h"

#include <mutex>
#include <utility>

namespace media
{
/**
 * @brief Thread-safe storage and updates for a track's metadata.
 *
 * MediaTrack owns a TrackInfo value. It does not process RTP or depend on
 * room membership, signaling, transports, or SFU endpoints.
 */
class MediaTrack
{
public:
    explicit MediaTrack(TrackInfo info)
        : info_(std::move(info))
    {
    }

    const std::string& id() const { return info_.sid;}

    TrackType type() const { return info_.type;}

    TrackSource source() const { return info_.source;}

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

    TrackInfo info() const
    {
        std::lock_guard<std::mutex> lock(mutex_);
        return info_;
    }

    void updateInfo(const TrackInfo& info)
    {
        std::lock_guard<std::mutex> lock(mutex_);
        info_ = info;
    }

private:
    mutable std::mutex mutex_;
    TrackInfo info_;
};
}

#endif // PACKETIA_MEDIA_TRACK_H
