#pragma once

#include "AgentRoomClient.h"
#include <memory>

namespace service::ai
{
std::shared_ptr<AgentRoomClient> MakeAgentRoomClient();
}
