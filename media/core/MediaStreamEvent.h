#ifndef PACKETIA_MEDIA_STREAM_EVENT_H_
#define PACKETIA_MEDIA_STREAM_EVENT_H_

#include "MediaEvent.h"
#include "MediaStream.h"
#include <utility>

namespace media
{
// 注册变化通知，不代表首帧已到达。跨线程使用时复制快照；以 revision 处理乱序通知。
template <MediaEventType TypeValue>
struct MediaStreamEvent final : MediaEvent
{
    MediaStreamEvent(MediaStream::Snapshot value, uint64_t version)
        : MediaEvent(TypeValue, MediaEventLevel::Info), stream(std::move(value)), revision(version) {}
    static constexpr MediaEventType StaticType() { return TypeValue; }
    const char* Name() const override
    {
        return TypeValue == MediaEventType::StreamRegistered ? "StreamRegistered" : "StreamUnregistered";
    }
    MediaStream::Snapshot stream;
    uint64_t revision;
};

using StreamRegisteredEvent = MediaStreamEvent<MediaEventType::StreamRegistered>;
using StreamUnregisteredEvent = MediaStreamEvent<MediaEventType::StreamUnregistered>;
}

#endif
