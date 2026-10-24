#include "MetricsJsonEncoder.h"
#include "MonitorService.h"
#include "nlohmann/json.hpp"
#include <algorithm>

namespace server::api
{
namespace
{
using Json = nlohmann::json;
Json Distribution(const monitoring::Histogram& value)
{
    return {{"count", value.Count()}, {"mean_us", value.MeanUs()}, {"max_us", value.MaxUs()},
        {"p50_us", value.PercentileUs(50)}, {"p95_us", value.PercentileUs(95)}, {"p99_us", value.PercentileUs(99)}};
}
double AgeMs(uint64_t now, uint64_t start) { return start && now > start ? (now - start) / 1000000.0 : 0; }
template<class T> Json Value(const std::optional<T>& value) { return value ? Json(*value) : Json(nullptr); }
}
std::string EncodeMetricsJson(const service::MonitorSnapshot& snapshot, const service::ServiceHealth& health)
{
    const auto& runtime = snapshot.runtime;
    const auto now = runtime.captured_ns;
    Json result = {{"schema_version", 1}, {"rate_window_seconds", 5}, {"workers", Json::array()},
        {"send_queues", Json::array()}, {"send", Json::object()},
        {"monitor", {{"healthy", health.healthy}, {"error_code", health.error_code},
            {"captured_monotonic_ns", now}, {"snapshot_age_ms", AgeMs(monitoring::NowNs(), now)}}}};
    for (const auto& worker : runtime.workers)
    {
        Json types = Json::object();
        for (const auto& task : worker.task_types)
            if (task.queue_wait.Count()) types[task.name] = {{"queue_wait", Distribution(task.queue_wait)},
                {"execution", Distribution(task.execution)}};
        result["workers"].push_back({{"pool", worker.pool}, {"index", worker.index},
            {"queue_depth", worker.queue_depth}, {"queue_bytes", worker.queue_bytes}, {"dropped_total", worker.dropped},
            {"oldest_job_wait_ms", AgeMs(now, worker.oldest_ns)}, {"active", worker.active_ns != 0},
            {"active_task_ms", AgeMs(now, worker.active_ns)}, {"busy_percent", worker.busy_percent},
            {"queue_wait", Distribution(worker.queue_wait)}, {"execution", Distribution(worker.execution)},
            {"task_types", std::move(types)}});
    }
    uint64_t bytes = 0, packets = 0;
    double oldest_ms = 0;
    for (const auto& queue : runtime.queues)
    {
        bytes += queue.bytes; packets += queue.packets;
        oldest_ms = std::max(oldest_ms, AgeMs(now, queue.oldest_ns));
        result["send_queues"].push_back({{"id", queue.id}, {"transport", queue.transport}, {"bytes", queue.bytes},
            {"packets", queue.packets}, {"oldest_wait_ms", AgeMs(now, queue.oldest_ns)}});
    }
    result["send_queue"] = {{"bytes", bytes}, {"packets", packets}, {"oldest_wait_ms", oldest_ms}};
    for (size_t i = 0; i < runtime.send.size(); ++i)
    {
        const auto& send = runtime.send[i];
        result["send"][i == 0 ? "tcp" : "udp"] = {{"bytes_total", send.bytes}, {"packets_total", send.packets},
            {"failures_total", send.failures}, {"blocked_total", send.blocked}, {"rejected_total", send.rejected},
            {"abandoned_packets_total", send.abandoned}, {"bitrate_bps", send.bitrate_bps}, {"packet_rate_pps", send.packet_rate_pps}};
    }
    const auto& r = snapshot.resources;
    result["resources"] = {{"logical_cpus", r.logical_cpus}, {"process_cpu_percent", Value(r.process_cpu_percent)},
        {"host_cpu_percent", Value(r.host_cpu_percent)}, {"process_cpu_seconds", Value(r.process_cpu_seconds)},
        {"process_rss_bytes", Value(r.process_rss_bytes)}, {"host_memory_total_bytes", Value(r.host_memory_total_bytes)},
        {"host_memory_available_bytes", Value(r.host_memory_available_bytes)}};
    return result.dump();
}
}
