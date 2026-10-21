#include "EndpointBase.h"
#include "logger.h"
#include <chrono>
#include <vector>

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
    Dispatch(job, std::nullopt);
}

void EndpointJobHandler::handle(WorkJob& job, std::size_t worker_index)
{
    Dispatch(job, worker_index);
}

void EndpointJobHandler::Dispatch(WorkJob& job, std::optional<std::size_t> worker_index)
{
    if (!mgr_)
    {
        LOG_INFO("ERROR mgr_");
        return;
    }

    const uint64_t endpoint_id = job.target_id != 0 ? job.target_id : job.key;
    auto endpoint = mgr_->Find(endpoint_id);
    if (!endpoint)
    {
        LOG_ERROR("endpoint not found, target_id=", endpoint_id,
                  " affinity_key=", job.key);
        return;
    }

    endpoint->ProcessJob(job);
    if (worker_index && job.type == WorkType::Rtp && endpoint->IsRunning()) {
        std::lock_guard<std::mutex> lock(streams_mutex_);
        worker_streams_[*worker_index].streams[{endpoint_id, job.key}] = endpoint;
    }
}

void EndpointJobHandler::on_worker_tick(std::size_t worker_index)
{
    const uint64_t now_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count();
    std::vector<std::pair<StreamKey, std::weak_ptr<EndpointBase>>> streams;
    {
        std::lock_guard<std::mutex> lock(streams_mutex_);
        const auto found = worker_streams_.find(worker_index);
        if (found == worker_streams_.end()) return;
        auto& state = found->second;
        if (now_ms - state.last_tick_ms < worker_tick_interval_ms()) return;
        state.last_tick_ms = now_ms;
        streams.assign(state.streams.begin(), state.streams.end());
    }
    for (const auto& entry : streams) {
        const auto endpoint = entry.second.lock();
        if (endpoint && mgr_->Find(entry.first.first) == endpoint && endpoint->IsRunning() &&
            endpoint->PollMedia(entry.first.second, now_ms)) continue;
        std::lock_guard<std::mutex> lock(streams_mutex_);
        worker_streams_[worker_index].streams.erase(entry.first);
    }
}

void EndpointJobHandler::on_worker_stop(std::size_t worker_index)
{
    std::lock_guard<std::mutex> lock(streams_mutex_);
    worker_streams_.erase(worker_index);
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
    case WorkType::Stun:
        OnStun(job);
        break;
    case WorkType::Dtls:
        OnDtls(job);
        break;
    default:
        LOG_ERROR("unsupported job type, key=", job.key, " type=", static_cast<int>(job.type));
        break;
    }
}

}
