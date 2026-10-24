#include "RuntimeMetrics.h"
#include <algorithm>
#include <atomic>
#include <functional>
#include <stdexcept>
#include <thread>
#include <utility>

namespace monitoring
{
namespace
{
constexpr uint64_t kSecond = 1000000000, kWindow = 5 * kSecond;
std::shared_ptr<MetricRegistry> active_registry;
template<class T> void Prune(std::vector<std::weak_ptr<T>>& values)
{
    values.erase(std::remove_if(values.begin(), values.end(), [](const auto& value) {
        return value.expired();
    }), values.end());
}
struct Traffic
{
    std::mutex mutex;
    TrafficSnapshot totals;
    std::array<uint64_t, 6> seconds{}, bytes{}, packets{};
};
}
struct MetricRegistry::Impl
{
    mutable std::mutex mutex;
    std::vector<std::weak_ptr<WorkerMetrics>> workers;
    std::vector<std::weak_ptr<SendQueueMetrics>> queues;
    uint64_t next_queue = 0;
    std::array<std::array<Traffic, 32>, 2> traffic;
};
WorkerMetrics::WorkerMetrics(std::string pool, size_t index, std::vector<std::string> names)
{
    state_.pool = std::move(pool); state_.index = index;
    for (auto& name : names) state_.task_types.push_back({std::move(name), {}, {}});
}
void WorkerMetrics::Queue(size_t depth, size_t bytes, uint64_t oldest, uint64_t dropped)
{
    std::lock_guard<std::mutex> lock(mutex_);
    state_.queue_depth = depth; state_.queue_bytes = bytes;
    state_.oldest_ns = oldest; state_.dropped = dropped;
}
void WorkerMetrics::Begin(uint64_t enqueued, size_t type, uint64_t now)
{
    std::lock_guard<std::mutex> lock(mutex_);
    state_.active_ns = now;
    const auto wait = enqueued && now >= enqueued ? now - enqueued : 0;
    state_.queue_wait.Add(wait);
    if (type < state_.task_types.size()) state_.task_types[type].queue_wait.Add(wait);
}
void WorkerMetrics::End(size_t type, uint64_t now)
{
    std::lock_guard<std::mutex> lock(mutex_);
    state_.execution.Add(now - state_.active_ns);
    if (type < state_.task_types.size()) state_.task_types[type].execution.Add(now - state_.active_ns);
    busy_.Add(state_.active_ns, now);
    state_.active_ns = 0;
}
WorkerSnapshot WorkerMetrics::Snapshot(uint64_t now) const
{
    WorkerSnapshot snapshot;
    BusyWindow busy;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        snapshot = state_;
        busy = busy_;
    }
    if (snapshot.active_ns) busy.Add(snapshot.active_ns, now);
    snapshot.busy_percent = std::min(100.0, 100.0 * busy.Read(now) / kWindow);
    return snapshot;
}
SendQueueMetrics::SendQueueMetrics(uint64_t id, std::string transport)
{ state_.id = id; state_.transport = std::move(transport); }
void SendQueueMetrics::Set(uint64_t bytes, uint64_t packets, uint64_t oldest)
{
    std::lock_guard<std::mutex> lock(mutex_);
    state_.bytes = bytes; state_.packets = packets; state_.oldest_ns = oldest;
}
SendQueueSnapshot SendQueueMetrics::Snapshot() const
{ std::lock_guard<std::mutex> lock(mutex_); return state_; }
MetricRegistry::MetricRegistry() : impl_(std::make_unique<Impl>()) {}
MetricRegistry::~MetricRegistry() = default;
std::shared_ptr<WorkerMetrics> MetricRegistry::RegisterWorker(const std::string& pool, size_t index,
                                                            std::vector<std::string> names)
{
    auto value = std::make_shared<WorkerMetrics>(pool, index, std::move(names));
    std::lock_guard<std::mutex> lock(impl_->mutex);
    Prune(impl_->workers);
    impl_->workers.push_back(value);
    return value;
}
std::shared_ptr<SendQueueMetrics> MetricRegistry::RegisterSendQueue(const std::string& transport)
{
    std::lock_guard<std::mutex> lock(impl_->mutex);
    auto value = std::make_shared<SendQueueMetrics>(++impl_->next_queue, transport);
    if (impl_->next_queue % 256 == 0) Prune(impl_->queues);
    impl_->queues.push_back(value);
    return value;
}
void MetricRegistry::RecordSend(Transport transport, SendEvent event, uint64_t bytes, uint64_t packets)
{
    thread_local const size_t index = std::hash<std::thread::id>{}(std::this_thread::get_id()) % 32;
    auto& shard = impl_->traffic[static_cast<size_t>(transport)][index];
    std::lock_guard<std::mutex> lock(shard.mutex);
    auto& total = shard.totals;
    switch (event)
    {
    case SendEvent::Sent:
    {
        total.bytes += bytes; total.packets += packets;
        const auto second = NowNs() / kSecond, slot = second % shard.seconds.size();
        if (shard.seconds[slot] != second)
        { shard.seconds[slot] = second; shard.bytes[slot] = shard.packets[slot] = 0; }
        shard.bytes[slot] += bytes; shard.packets[slot] += packets;
        break;
    }
    case SendEvent::Failed: ++total.failures; break;
    case SendEvent::Blocked: ++total.blocked; if (transport == Transport::Udp) ++total.failures; break;
    case SendEvent::Rejected: ++total.rejected; break;
    case SendEvent::Abandoned: total.abandoned += packets; break;
    }
}
RuntimeSnapshot MetricRegistry::Collect() const
{
    std::vector<std::shared_ptr<WorkerMetrics>> workers;
    std::vector<std::shared_ptr<SendQueueMetrics>> queues;
    {
        std::lock_guard<std::mutex> lock(impl_->mutex);
        Prune(impl_->workers); Prune(impl_->queues);
        for (auto& weak : impl_->workers) if (auto value = weak.lock()) workers.push_back(std::move(value));
        for (auto& weak : impl_->queues) if (auto value = weak.lock()) queues.push_back(std::move(value));
    }
    RuntimeSnapshot result;
    result.captured_ns = NowNs();
    for (auto& worker : workers) result.workers.push_back(worker->Snapshot(result.captured_ns));
    std::sort(result.workers.begin(), result.workers.end(), [](const auto& a, const auto& b) {
        return a.pool == b.pool ? a.index < b.index : a.pool < b.pool;
    });
    for (auto& queue : queues)
    {
        auto snapshot = queue->Snapshot();
        if (snapshot.packets) result.queues.push_back(std::move(snapshot));
    }
    const auto second = result.captured_ns / kSecond;
    for (size_t transport = 0; transport < 2; ++transport)
    {
        auto& total = result.send[transport];
        uint64_t recent_bytes = 0, recent_packets = 0;
        for (auto& shard : impl_->traffic[transport])
        {
            std::lock_guard<std::mutex> lock(shard.mutex);
            total.bytes += shard.totals.bytes; total.packets += shard.totals.packets;
            total.failures += shard.totals.failures; total.blocked += shard.totals.blocked;
            total.rejected += shard.totals.rejected; total.abandoned += shard.totals.abandoned;
            for (size_t i = 0; i < shard.seconds.size(); ++i)
                if (shard.seconds[i] <= second && second - shard.seconds[i] < 5)
                { recent_bytes += shard.bytes[i]; recent_packets += shard.packets[i]; }
        }
        total.bitrate_bps = recent_bytes * 8.0 / 5; total.packet_rate_pps = recent_packets / 5.0;
    }
    return result;
}
MetricsContext::MetricsContext(bool activate) : registry_(std::make_shared<MetricRegistry>())
{
    if (activate && !Activate())
        throw std::logic_error("only one application metrics context may be active");
}
MetricsContext::~MetricsContext() { Deactivate(); }
bool MetricsContext::Activate()
{
    std::shared_ptr<MetricRegistry> expected;
    return std::atomic_compare_exchange_strong(&active_registry, &expected, registry_) || expected == registry_;
}
void MetricsContext::Deactivate()
{
    auto expected = registry_;
    std::atomic_compare_exchange_strong(&active_registry, &expected, std::shared_ptr<MetricRegistry>{});
}
std::shared_ptr<WorkerMetrics> RegisterWorker(const std::string& pool, size_t index, std::vector<std::string> names)
{
    auto registry = std::atomic_load(&active_registry);
    return registry ? registry->RegisterWorker(pool, index, std::move(names)) : nullptr;
}
std::shared_ptr<SendQueueMetrics> RegisterSendQueue(const std::string& transport)
{
    auto registry = std::atomic_load(&active_registry);
    return registry ? registry->RegisterSendQueue(transport) : nullptr;
}
void RecordSend(Transport transport, SendEvent event, uint64_t bytes, uint64_t packets)
{
    auto registry = std::atomic_load(&active_registry);
    if (registry) registry->RecordSend(transport, event, bytes, packets);
}
TaskScope::TaskScope(WorkerMetrics* worker, uint64_t enqueued, size_t type) : worker_(worker), type_(type)
{ if (worker_) worker_->Begin(enqueued, type, NowNs()); }
TaskScope::~TaskScope() { if (worker_) worker_->End(type_, NowNs()); }
}
