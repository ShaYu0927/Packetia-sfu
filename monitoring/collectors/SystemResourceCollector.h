#pragma once
#include <cstdint>
#include <optional>

namespace monitoring
{
struct SystemResources
{
    unsigned logical_cpus = 0;
    std::optional<double> process_cpu_percent, host_cpu_percent, process_cpu_seconds;
    std::optional<uint64_t> process_rss_bytes, host_memory_total_bytes, host_memory_available_bytes;
};
struct ResourceReading
{
    std::optional<double> process_cpu_seconds;
    std::optional<uint64_t> rss_bytes, host_total_bytes, host_available_bytes;
    uint64_t host_total_ticks = 0, host_idle_ticks = 0;
};
// Owned and called serially by MonitorService, independent of HTTP and registry.
class SystemResourceCollector
{
public:
    SystemResources Collect(uint64_t now);
private:
    static ResourceReading Read();
    uint64_t last_ns_ = 0;
    ResourceReading previous_;
    std::optional<double> process_percent_, host_percent_;
};
}
