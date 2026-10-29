#pragma once
#include "SignalMessages.h"
#include "ParticipantTypes.h"

namespace server::signaling
{
Json ParticipantMessage(const room::ParticipantInfo& info);
room::ParticipantPermissions ParticipantPermissions(const service::auth::RoomGrant& grant);
// 只修改调用者的临时快照；所有字段校验成功后再由房间提交。
bool ApplyParticipantPatch(const Json& body, room::ParticipantInfo& info, Json& error);
}
