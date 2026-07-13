#ifndef _IWORKER_MOUDULE_H_
#define _IWORKER_MOUDULE_H_

#pragma once

#include "ShardedWorkerPool.h"
#include "EndpointBase.h"
#include <string>
#include <utility>

class WorkerModuleBase
{
public:
    WorkerModuleBase(WorkerService::WorkerPoolId id,
                     std::size_t worker_count,
                     std::size_t max_queue_len,
                     ShardedWorkerPool::DropPolicy drop_policy)
        : WorkerModuleBase(
              std::string(ToName(id)), worker_count, max_queue_len,
              drop_policy, false)
    {
    }

    WorkerModuleBase(std::string name,
                     std::size_t worker_count,
                     std::size_t max_queue_len,
                     ShardedWorkerPool::DropPolicy drop_policy,
                     bool function_pool)
        : name_(std::move(name)),
          worker_count_(worker_count),
          max_queue_len_(max_queue_len),
          drop_policy_(drop_policy),
          function_pool_(function_pool)
    {
    }

    virtual ~WorkerModuleBase() = default;

    int Register()
    {
        if (function_pool_)
        {
            return WorkerService::create_function_pool(
                name_, worker_count_, max_queue_len_, drop_policy_);
        }
        return WorkerService::create_pool(
            name_,
            worker_count_,
            CreateHandler(),
            max_queue_len_,
            drop_policy_);
    }

    void Unregister(bool drain = true) const
    {
        WorkerService::destroy_pool(name_, drain);
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
        case WorkerService::WorkerPoolId::Endpoint: return "endpoint_pool";
        default: return "unknown";
        }
    }

private:
    std::string name_;
    std::size_t worker_count_;
    std::size_t max_queue_len_;
    ShardedWorkerPool::DropPolicy drop_policy_;
    bool function_pool_ = false;
};

class FunctionWorkerModule : public WorkerModuleBase
{
public:
    FunctionWorkerModule(
        std::string name,
        std::size_t worker_count,
        std::size_t max_queue_len = 2048,
        ShardedWorkerPool::DropPolicy drop_policy =
            ShardedWorkerPool::DropPolicy::DropTail)
        : WorkerModuleBase(std::move(name), worker_count, max_queue_len,
                           drop_policy, true)
    {
    }

protected:
    std::shared_ptr<IJobHandler> CreateHandler() override
    {
        return nullptr;
    }
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

class EndpointWorkerModule : public WorkerModuleBase
{
public:
    EndpointWorkerModule()
        : WorkerModuleBase(
              WorkerService::WorkerPoolId::Endpoint,
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

    ~WorkerModuleRegistry()
    {
        UnregisterAll(true);
    }

    void Add(ModulePtr module);
    int RegisterAll();
    void UnregisterAll(bool drain = true);

private:
    std::vector<ModulePtr> modules_;
    std::vector<ModulePtr> registered_;
};

#endif /* _IWORKER_MOUDULE_H_ */
