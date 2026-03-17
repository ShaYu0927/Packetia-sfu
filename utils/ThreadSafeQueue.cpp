#include "ThreadSafeQueue.h"

void QueueManager::Start(size_t worker_num)
{
    queues.resize(worker_num);
        workers.reserve(worker_num);

        for (size_t i = 0; i < worker_num; ++i)
        {
            workers.emplace_back([this,i]{
                WorkerLoop(i);
            });
        }
}

void QueueManager::Post(size_t key, Task task)
{
    size_t idx = key % queues.size();
    queues[idx].push(std::move(task));
}