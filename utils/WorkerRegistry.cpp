#include "WorkerRegistry.h"
#include "logger.h"

#include <exception>
#include <stdexcept>
#include <utility>

WorkerRegistry::~WorkerRegistry()
{
    Stop();
}

void WorkerRegistry::Add(WorkerPoolConfig config, std::shared_ptr<IJobHandler> handler)
{
    AddEntry({std::move(config), std::move(handler), Kind::Jobs});
}

void WorkerRegistry::AddFunction(WorkerPoolConfig config)
{
    AddEntry({std::move(config), nullptr, Kind::Functions});
}

void WorkerRegistry::AddEntry(Entry entry)
{
    if (started_count_ != 0)
    {
        throw std::logic_error("cannot configure running worker pools");
    }
    pools_.push_back(std::move(entry));
}

int WorkerRegistry::Start()
{
    try
    {
        while (started_count_ < pools_.size())
        {
            const auto& entry = pools_[started_count_];
            const auto& config = entry.config;
            if (WorkerService::exists(config.name))
            {
                LOG_ERROR("[WorkerRegistry] pool already exists, name=", config.name);
                Stop();
                return -1;
            }

            const int result = entry.kind == Kind::Functions
                ? WorkerService::create_function_pool(config.name, config.worker_count,
                    config.max_queue_len, config.drop_policy)
                : WorkerService::create_pool(config.name, config.worker_count, entry.handler,
                    config.max_queue_len, config.drop_policy);
            if (result != 0)
            {
                LOG_ERROR("[WorkerRegistry] start failed, name=", config.name,
                          " result=", result);
                Stop();
                return result;
            }
            ++started_count_;
        }
        return 0;
    }
    catch (const std::exception& error)
    {
        LOG_ERROR("[WorkerRegistry] start failed, error=", error.what());
    }
    catch (...)
    {
        LOG_ERROR("[WorkerRegistry] start failed, error=unknown");
    }
    Stop();
    return -1;
}

void WorkerRegistry::Stop(bool drain)
{
    while (started_count_ != 0)
    {
        WorkerService::destroy_pool(pools_[--started_count_].config.name, drain);
    }
}
