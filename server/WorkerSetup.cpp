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
void AddWorkerPools(ServerLauncher& launcher, const config::WorkerOptions& options)
{
    using DropPolicy = ShardedWorkerPool::DropPolicy;
    auto workers = std::make_shared<WorkerRegistry>();
    const auto transcode_threads = options.transcode_threads ? options.transcode_threads
        : std::clamp<std::size_t>(std::thread::hardware_concurrency() / 2, 1, 4);

    // Pool name, worker count, queue length and overflow policy.
    workers->Add({POOL_MEDIA, options.media_threads, options.media_queue_len, DropPolicy::DropHead},
        std::make_shared<utils::EndpointJobHandler>(&utils::EndpointManager::Instance()));
    workers->Add({POOL_ENDPOINT, options.endpoint_threads, options.endpoint_queue_len, DropPolicy::DropHead},
        std::make_shared<utils::EndpointJobHandler>(&utils::EndpointManager::Instance()));
    workers->AddFunction({POOL_TRANSCODE, transcode_threads, options.transcode_queue_len, DropPolicy::DropTail});

    launcher.AddCustomService(SERVICE_WORKERS,
        [workers] { return workers->Start() == 0; },
        [workers] { workers->Stop(); });
}
}
