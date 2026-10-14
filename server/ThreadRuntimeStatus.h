#pragma once

#include "utils/ThreadManager.h"
#include "utils/TaskScheduler.h"
#include "utils/ShardedWorkerPool.h"

namespace server
{
// Process-wide thread/pool snapshots and this application's I/O schedulers.
struct ThreadRuntimeStatus
{
    std::vector<utils::ThreadStatus> threads;
    std::vector<WorkerService::PoolStatus> pools;
    std::vector<TaskScheduler::Status> io;
};
}
