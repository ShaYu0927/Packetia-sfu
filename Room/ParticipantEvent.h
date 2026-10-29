#pragma once
#include "MediaEvent.h"
#include "ParticipantTypes.h"
#include <utility>

namespace room
{
// 已提交的成员快照；不包含连接标识、恢复凭据或可调用的媒体对象。
class ParticipantChangedEvent final : public media::MediaEvent
{
public:
    static constexpr media::MediaEventType StaticType() { return media::MediaEventType::ParticipantChanged; }
    ParticipantChangedEvent(std::string room, uint64_t version, ParticipantInfo value, bool left)
        : MediaEvent(StaticType(), media::MediaEventLevel::Info), room_id(std::move(room)),
          revision(version), participant(std::move(value)), removed(left)
    { participant.signaling = {}; }
    const char* Name() const override { return "ParticipantChanged"; }
    const std::string room_id;
    const uint64_t revision;
    ParticipantInfo participant;
    const bool removed;
};
}
