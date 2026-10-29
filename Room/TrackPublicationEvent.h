#ifndef PACKETIA_ROOM_TRACK_PUBLICATION_EVENT_H_
#define PACKETIA_ROOM_TRACK_PUBLICATION_EVENT_H_

#include "MediaEvent.h"
#include "TrackPublicationInfo.h"

#include <utility>

namespace room
{
// 值快照不持有房间、端点或网络对象，允许复制后投递到其他执行器。
struct TrackPublicationSnapshot
{
    std::string room_id;
    std::string publisher_id;
    uint64_t publication_id = 0; // 房间实例内的发布实例标识，同名重新发布时变化。
    uint64_t revision = 0;       // 房间实例内的轨道变更版本。
    media::TrackPublicationInfo track;
};

class TrackPublishedEvent final : public media::MediaEvent
{
public:
    static constexpr media::MediaEventType StaticType()
    {
        return media::MediaEventType::TrackPublished;
    }

    explicit TrackPublishedEvent(TrackPublicationSnapshot value)
        : MediaEvent(StaticType(), media::MediaEventLevel::Info), snapshot(std::move(value))
    {
        Meta().track_id = snapshot.track.sid;
    }

    const char* Name() const override
    {
        return "TrackPublished";
    }

    const TrackPublicationSnapshot snapshot;
};

class TrackUnpublishedEvent final : public media::MediaEvent
{
public:
    static constexpr media::MediaEventType StaticType()
    {
        return media::MediaEventType::TrackUnpublished;
    }

    explicit TrackUnpublishedEvent(TrackPublicationSnapshot value)
        : MediaEvent(StaticType(), media::MediaEventLevel::Info), snapshot(std::move(value))
    {
        Meta().track_id = snapshot.track.sid;
    }

    const char* Name() const override
    {
        return "TrackUnpublished";
    }

    const TrackPublicationSnapshot snapshot;
};
}

#endif
