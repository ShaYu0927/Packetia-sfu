#pragma once
#include <array>
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace monitoring
{
uint64_t NowNs() noexcept;
class Histogram
{
public:
    void Add(uint64_t ns) noexcept;
    uint64_t Count() const noexcept { return count_; }
    double MeanUs() const noexcept { return count_ ? static_cast<double>(sum_ns_ / count_ / 1000) : 0; }
    double MaxUs() const noexcept { return max_ns_ / 1000.0; }
    uint64_t PercentileUs(unsigned percentile) const noexcept;
private:
    std::array<uint64_t, 258> buckets_{};
    uint64_t count_ = 0, max_ns_ = 0;
    long double sum_ns_ = 0;
};
class BusyWindow
{
public:
    void Add(uint64_t begin, uint64_t end) noexcept;
    uint64_t Read(uint64_t now) const noexcept;
private:
    std::array<uint64_t, 6> seconds_{}, busy_ns_{};
};
enum class Transport { Tcp, Udp };
enum class SendEvent { Sent, Failed, Blocked, Rejected, Abandoned };
struct TaskSnapshot { std::string name; Histogram queue_wait, execution; };
struct WorkerSnapshot
{
    std::string pool;
    size_t index = 0, queue_depth = 0, queue_bytes = 0;
    uint64_t dropped = 0, oldest_ns = 0, active_ns = 0;
    double busy_percent = 0;
    Histogram queue_wait, execution;
    std::vector<TaskSnapshot> task_types;
};
struct SendQueueSnapshot
{
    uint64_t id = 0;
    std::string transport;
    uint64_t bytes = 0, packets = 0, oldest_ns = 0;
};
struct TrafficSnapshot
{
    uint64_t bytes = 0, packets = 0, failures = 0, blocked = 0, rejected = 0, abandoned = 0;
    double bitrate_bps = 0, packet_rate_pps = 0;
};
// Values own their data. Consumers may calculate/encode without producer locks.
struct RuntimeSnapshot
{
    uint64_t captured_ns = 0;
    std::vector<WorkerSnapshot> workers;
    std::vector<SendQueueSnapshot> queues;
    std::array<TrafficSnapshot, 2> send{};
};
}
