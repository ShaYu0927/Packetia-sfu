#pragma once

#include "AgentWorker.h"
#include <memory>

namespace service::ai
{
class AgentRoomClient;

std::shared_ptr<AgentWorker> MakeLocalAgentWorker(std::shared_ptr<AgentRoomClient> room_client);
}
