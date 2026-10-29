#ifndef PACKETIA_MEDIA_TRACK_EVENT_H_
#define PACKETIA_MEDIA_TRACK_EVENT_H_

#include "MediaEvent.h"
#include "MediaTrackSubscription.h"

namespace media
{
// 管理器提交后的可用性通知；不代表媒体首帧或协议连接状态。
struct MediaTrackStateEvent final : MediaEvent
{
    MediaTrackStateEvent(MediaTrackRef value, MediaTrackState status, uint64_t version)
        : MediaEvent(StaticType(), MediaEventLevel::Info), track(value), state(status), revision(version) {}
    static constexpr MediaEventType StaticType() { return MediaEventType::TrackAvailabilityChanged; }
    const char* Name() const override { return "TrackAvailabilityChanged"; }
    MediaTrackRef track;
    MediaTrackState state;
    uint64_t revision;
};
}
#endif
