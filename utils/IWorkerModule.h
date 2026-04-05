#ifndef _IWORKER_MOUDULE_H_
#define _IWORKER_MOUDULE_H_

#pragma once

#include "ShardedWorkerPool.h"
#include "EndpointBase.h"
#include <string>

class WorkerModuleBase
{
public:
    WorkerModuleBase(WorkerService::WorkerPoolId id,
                     std::size_t worker_count,
                     std::size_t max_queue_len,
                     ShardedWorkerPool::DropPolicy drop_policy)
        : id_(id),
          worker_count_(worker_count),
          max_queue_len_(max_queue_len),
          drop_policy_(drop_policy)
    {
    }

    virtual ~WorkerModuleBase() = default;

    int Register()
    {
        return WorkerService::create_pool(
            ToName(id_),
            worker_count_,
            CreateHandler(),
            max_queue_len_,
            drop_policy_);
    }

    void Unregister(bool drain = true) const
    {
        WorkerService::destroy_pool(ToName(id_), drain);
    }

protected:
    virtual std::shared_ptr<IJobHandler> CreateHandler() = 0;

    static const char* ToName(WorkerService::WorkerPoolId id)
    {
        switch (id)
        {
        case WorkerService::WorkerPoolId::Media: return "media";
        case WorkerService::WorkerPoolId::Sip:   return "sip";
        case WorkerService::WorkerPoolId::Rtsp:  return "rtsp";
        default: return "unknown";
        }
    }

private:
    WorkerService::WorkerPoolId id_;
    std::size_t worker_count_;
    std::size_t max_queue_len_;
    ShardedWorkerPool::DropPolicy drop_policy_;
};

class MediaWorkerModule : public WorkerModuleBase
{
public:
    MediaWorkerModule()
        : WorkerModuleBase(
              WorkerService::WorkerPoolId::Media,
              4,
              4096,
              ShardedWorkerPool::DropPolicy::DropHead)
    {
    }

protected:
    std::shared_ptr<IJobHandler> CreateHandler() override
    {
        return std::make_shared<utils::EndpointJobHandler>(
            &utils::EndpointManager::Instance());
    }
};

class WorkerModuleRegistry
{
public:
    using ModulePtr = std::shared_ptr<WorkerModuleBase>;

    void Add(ModulePtr module);
    int RegisterAll();
    void UnregisterAll(bool drain = true);

private:
    std::vector<ModulePtr> modules_;
    std::vector<ModulePtr> registered_;
};

#endif /* _IWORKER_MOUDULE_H_ */