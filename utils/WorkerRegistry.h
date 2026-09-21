#ifndef PACKETIA_UTILS_WORKERREGISTRY_H_
#define PACKETIA_UTILS_WORKERREGISTRY_H_

#include "ShardedWorkerPool.h"

#include <cstddef>
#include <memory>
#include <string>
#include <vector>

struct WorkerPoolConfig
{
    std::string name;
    std::size_t worker_count;
    std::size_t max_queue_len;
    ShardedWorkerPool::DropPolicy drop_policy;
};

// The owning service serializes configuration, Start and Stop. Registered pool
// names are exclusive to that owner until Stop has drained its workers.
class WorkerRegistry final
{
public:
    WorkerRegistry() = default;
    ~WorkerRegistry();
    WorkerRegistry(const WorkerRegistry&) = delete;
    WorkerRegistry& operator=(const WorkerRegistry&) = delete;

    void Add(WorkerPoolConfig config, std::shared_ptr<IJobHandler> handler);
    void AddFunction(WorkerPoolConfig config);

    // Start is idempotent; failure rolls back pools started by this registry.
    int Start();
    // Stop in reverse order. Configuration is retained for a later Start.
    void Stop(bool drain = true);

private:
    enum class Kind { Jobs, Functions };
    struct Entry
    {
        WorkerPoolConfig config;
        std::shared_ptr<IJobHandler> handler;
        Kind kind;
    };

    void AddEntry(Entry entry);

    std::vector<Entry> pools_;
    std::size_t started_count_ = 0;
};

#endif // PACKETIA_UTILS_WORKERREGISTRY_H_
