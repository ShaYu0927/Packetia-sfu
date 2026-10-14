#include "ThreadSafeQueue.h"

#include <stdexcept>

namespace { constexpr const char* kQueuePool = "legacy_queue"; }

void QueueManager::Start(size_t worker_num)
{
    std::lock_guard<std::mutex> lock(lifecycle_mutex_);
    if (started_) return;
    if (!worker_num) throw std::invalid_argument("QueueManager needs at least one worker");
    auto registry = std::make_unique<WorkerRegistry>();
    registry->AddFunction({kQueuePool, worker_num, 4096, ShardedWorkerPool::DropPolicy::DropTail});
    if (registry->Start() != 0) throw std::runtime_error("QueueManager worker startup failed");
    workers_ = std::move(registry);
    started_ = true;
}

void QueueManager::Stop()
{
    std::lock_guard<std::mutex> lock(lifecycle_mutex_);
    started_ = false;
    if (workers_) workers_->Stop();
    workers_.reset();
}

bool QueueManager::TryPost(size_t key, Task task)
{
    return started_ && WorkerService::post_fn(kQueuePool, key, std::move(task)) == 0;
}

void QueueManager::Post(size_t key, Task task) { TryPost(key, std::move(task)); }
