#ifndef PACKETIA_TRACK_PUBLICATION_EVENT_H_
#define PACKETIA_TRACK_PUBLICATION_EVENT_H_

#include "MediaEvent.h"
#include "TrackMetadata.h"
#include <utility>

namespace media
{
// 值快照不持有房间、端点或网络对象，允许复制后投递到其他执行器。
struct TrackPublicationSnapshot
{
    std::string room_id;
    std::string publisher_id;
    uint64_t publication_id = 0; // 房间实例内的发布实例标识，同名重新发布时变化。
    uint64_t revision = 0;       // 房间实例内的轨道变更版本。
    TrackMetadata track;
};

class TrackPublishedEvent final : public MediaEvent
{
public:
    static constexpr MediaEventType StaticType() { return MediaEventType::TrackPublished; }
    explicit TrackPublishedEvent(TrackPublicationSnapshot value)
        : MediaEvent(StaticType(), MediaEventLevel::Info), snapshot(std::move(value))
    { Meta().track_id = snapshot.track.sid; }
    const char* Name() const override { return "TrackPublished"; }
    const TrackPublicationSnapshot snapshot;
};

class TrackUnpublishedEvent final : public MediaEvent
{
public:
    static constexpr MediaEventType StaticType() { return MediaEventType::TrackUnpublished; }
    explicit TrackUnpublishedEvent(TrackPublicationSnapshot value)
        : MediaEvent(StaticType(), MediaEventLevel::Info), snapshot(std::move(value))
    { Meta().track_id = snapshot.track.sid; }
    const char* Name() const override { return "TrackUnpublished"; }
    const TrackPublicationSnapshot snapshot;
};
}

#endif
