#ifndef PACKETIA_MEDIA_TRACK_TRACK_PUBLICATION_H_
#define PACKETIA_MEDIA_TRACK_TRACK_PUBLICATION_H_

#include "TrackPublicationFwd.h"
#include "TrackPublicationInfo.h"

#include <mutex>
#include <utility>

namespace media
{
// 发布元数据的线程安全存储；不表示媒体接收器就绪，不处理 RTP 或创建订阅关系。
class TrackPublication
{
public:
    explicit TrackPublication(TrackPublicationInfo info)
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

    TrackPublicationInfo info() const
    {
        std::lock_guard<std::mutex> lock(mutex_);
        return info_;
    }

    void updateInfo(const TrackPublicationInfo& info)
    {
        std::lock_guard<std::mutex> lock(mutex_);
        info_ = info;
    }

private:
    mutable std::mutex mutex_;
    TrackPublicationInfo info_;
};
}

#endif // PACKETIA_MEDIA_TRACK_TRACK_PUBLICATION_H_
