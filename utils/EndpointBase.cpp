#include "EndpointBase.h"
#include "logger.h"

namespace utils 
{
std::uint64_t EndpointManager::AllocId()
{
    return next_id_.fetch_add(1, std::memory_order_relaxed);
}

bool EndpointManager::Add(const std::shared_ptr<EndpointBase>& endpoint)
{
    if (!endpoint)
    {
        return false;
    }

    std::lock_guard<std::mutex> lock(mtx_);
    return endpoints_.emplace(endpoint->Id(), endpoint).second; 
}

std::shared_ptr<EndpointBase> EndpointManager::Find(std::uint64_t endpoint_id)
{
    std::lock_guard<std::mutex> lock(mtx_);
    auto it = endpoints_.find(endpoint_id);
    if (it == endpoints_.end())
    {
        return nullptr;
    }
    return it->second;
}

bool EndpointManager::Remove(std::uint64_t endpoint_id)
{
    std::lock_guard<std::mutex> lock(mtx_);
    return endpoints_.erase(endpoint_id) > 0;
}

bool EndpointManager::Exists(std::uint64_t endpoint_id)
{
    std::lock_guard<std::mutex> lock(mtx_);
    return endpoints_.find(endpoint_id) != endpoints_.end();
}

std::size_t EndpointManager::Size() const
{
    std::lock_guard<std::mutex> lock(mtx_);
    return endpoints_.size();
}

void EndpointManager::Clear()
{
    std::lock_guard<std::mutex> lock(mtx_);
    endpoints_.clear();
}

void EndpointJobHandler::handle(WorkJob& job)
{
    if (!mgr_)
    {
        if (job.deleter) job.deleter(job);
        LOG_INFO("ERROR mgr_");
        return;
    }

    endpoint = mgr_->Find(job.key);
    if (!endpoint)
    {
        if (job.deleter) job.deleter(job);
        LOG_ERROR("endpoint not found, key=", job.key);
        return;
    }

    endpoint->ProcessJob(job);

    if (job.deleter)
    {
        job.deleter(job);
    }
}

void EndpointBase::ProcessJob(WorkJob& job)
{
    switch (job.type)
    {
    case WorkType::Rtp:
        OnRtp(job);
        break;
    case WorkType::Rtcp:
        OnRtcp(job);
        break;
    default:
        LOG_ERROR("unsupported job type, key=", job.key, " type=", static_cast<int>(job.type));
        break;
    }
}

}
