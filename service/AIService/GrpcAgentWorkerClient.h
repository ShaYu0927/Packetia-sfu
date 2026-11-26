#pragma once

#include "AgentWorker.h"
#include <memory>

namespace service::ai
{
std::shared_ptr<AgentWorker> MakeGrpcAgentWorkerClient(std::string address);
}
