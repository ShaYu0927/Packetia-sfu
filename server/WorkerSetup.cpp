#include "WorkerSetup.h"
#include "ServerLauncher.h"
#include "ServiceNames.h"
#include "WorkerRegistry.h"
#include "EndpointBase.h"

#include <algorithm>
#include <memory>
#include <thread>

namespace server
{
void AddWorkerPools(ServerLauncher& launcher)
{
    using DropPolicy = ShardedWorkerPool::DropPolicy;
    auto workers = std::make_shared<WorkerRegistry>();
    const auto transcode_threads = std::clamp<std::size_t>(
        std::thread::hardware_concurrency() / 2, 1, 4);

    // Pool name, worker count, queue length and overflow policy.
    workers->Add({POOL_MEDIA, 4, 4096, DropPolicy::DropHead},
        std::make_shared<utils::EndpointJobHandler>(&utils::EndpointManager::Instance()));
    workers->Add({POOL_ENDPOINT, 4, 4096, DropPolicy::DropHead},
        std::make_shared<utils::EndpointJobHandler>(&utils::EndpointManager::Instance()));
    workers->AddFunction({POOL_TRANSCODE, transcode_threads, 512, DropPolicy::DropTail});

    launcher.AddCustomService(SERVICE_WORKERS,
        [workers] { return workers->Start() == 0; },
        [workers] { workers->Stop(); });
}
}
