#include "MediaLatency.h"
#include "logger.h"

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <iomanip>
#include <limits>
#include <sstream>
#include <thread>

namespace media_latency
{
namespace
{
uint64_t ReadSetting(const char* name, uint64_t fallback, uint64_t maximum) noexcept
{
    const char* value = std::getenv(name);
    if (!value || !*value) return fallback;
    uint64_t parsed = 0;
    for (; *value; ++value)
    {
        if (*value < '0' || *value > '9' ||
            parsed > (maximum - static_cast<unsigned>(*value - '0')) / 10)
            return fallback;
        parsed = parsed * 10 + static_cast<unsigned>(*value - '0');
    }
    return parsed <= maximum ? parsed : fallback;
}

struct Settings
{
    uint64_t sample_every = ReadSetting("PACKETIA_MEDIA_LATENCY_SAMPLE_EVERY", 1, 1000000);
    uint64_t interval_ns = std::max<uint64_t>(
        1, ReadSetting("PACKETIA_MEDIA_LATENCY_INTERVAL_MS", 5000, 3600000)) * 1000000;
};

const Settings& Config() noexcept
{
    static const Settings settings;
    return settings;
}

constexpr auto BucketBounds()
{
    std::array<uint64_t, 258> bounds{};
    for (std::size_t i = 0; i <= 16; ++i) bounds[i] = i;
    std::size_t index = 17;
    for (unsigned exponent = 1; exponent <= 30; ++exponent)
        for (uint64_t part = 1; part <= 8; ++part)
            bounds[index++] = (8 + part) * (uint64_t{1} << exponent);
    bounds.back() = std::numeric_limits<uint64_t>::max();
    return bounds;
}
constexpr auto kBounds = BucketBounds();
uint64_t CeilUs(uint64_t ns) noexcept { return ns / 1000 + (ns % 1000 != 0); }

struct LocalState
{
    Snapshot stats;
    PacketTrace packet;
    SendTrace send;
    uint64_t sample_counter = 0;
    uint64_t last_report_ns = 0;
};
thread_local LocalState local;

constexpr const char* kStageNames[] = {
    "ingress", "worker_queue", "before_forward", "fanout",
    "udp_send", "udp_total", "tcp_send", "tcp_total"
};
constexpr const char* kCounterNames[] = {
    "ingress_sampled", "worker_started", "queue_dropped", "forward_inputs",
    "no_subscribers", "sender_attempts", "sender_rejected", "udp_sent",
    "udp_failed", "tcp_sent", "tcp_abandoned"
};
}

uint64_t NowNs() noexcept
{
    return static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count());
}

void Histogram::Add(uint64_t ns) noexcept
{
    const auto index = std::lower_bound(kBounds.begin(), kBounds.end(), CeilUs(ns)) - kBounds.begin();
    ++buckets_[static_cast<std::size_t>(index)];
    ++count_;
    sum_ns_ += ns;
    max_ns_ = std::max(max_ns_, ns);
}

double Histogram::MeanUs() const noexcept
{
    return count_ ? static_cast<double>(sum_ns_ / count_ / 1000.0L) : 0;
}

uint64_t Histogram::PercentileUs(unsigned percentile) const noexcept
{
    if (!count_) return 0;
    percentile = std::clamp(percentile, 1U, 100U);
    const auto rank = (count_ / 100) * percentile +
        ((count_ % 100) * percentile + 99) / 100;
    uint64_t seen = 0;
    for (std::size_t i = 0; i < buckets_.size(); ++i)
    {
        seen += buckets_[i];
        if (seen >= rank) return std::min(kBounds[i], CeilUs(max_ns_));
    }
    return CeilUs(max_ns_);
}

PacketTrace BeginPacket() noexcept
{
    const auto every = Config().sample_every;
    if (every == 0 || local.sample_counter++ % every != 0) return {};
    Count(Counter::IngressSampled);
    return {NowNs(), 0, 0};
}

void Count(Counter counter) noexcept { ++local.stats.counters[static_cast<std::size_t>(counter)]; }

void Observe(Stage stage, uint64_t start_ns, uint64_t end_ns) noexcept
{
    if (start_ns && end_ns >= start_ns)
        local.stats.stages[static_cast<std::size_t>(stage)].Add(end_ns - start_ns);
}

void WorkerStarted(PacketTrace& packet) noexcept
{
    if (!packet) return;
    packet.worker_ns = NowNs();
    Count(Counter::WorkerStarted);
    Observe(Stage::Ingress, packet.ingress_ns, packet.enqueue_ns);
    Observe(Stage::WorkerQueue, packet.enqueue_ns, packet.worker_ns);
}

void SocketSent(const SendTrace& trace, bool tcp, uint64_t completed_ns) noexcept
{
    if (!trace || !trace.packet || trace.packet.ingress_ns > trace.submitted_ns ||
        completed_ns < trace.submitted_ns) return;
    Count(tcp ? Counter::TcpSent : Counter::UdpSent);
    Observe(tcp ? Stage::TcpSend : Stage::UdpSend, trace.submitted_ns, completed_ns);
    Observe(tcp ? Stage::TcpTotal : Stage::UdpTotal, trace.packet.ingress_ns, completed_ns);
}

PacketTrace CurrentPacket() noexcept { return local.packet; }
SendTrace CurrentSend() noexcept { return local.send; }

PacketScope::PacketScope(PacketTrace packet) noexcept
    : previous_packet_(local.packet), previous_send_(local.send)
{
    local.packet = packet;
    local.send = {};
}
PacketScope::~PacketScope() { local.packet = previous_packet_; local.send = previous_send_; }

SendScope::SendScope(bool retransmit) noexcept : previous_(local.send)
{
    local.send = !retransmit && local.packet && local.packet.worker_ns
        ? SendTrace{local.packet, NowNs()} : SendTrace{};
}
SendScope::~SendScope() { local.send = previous_; }

Snapshot TakeSnapshot() noexcept
{
    auto snapshot = local.stats;
    local.stats = {};
    return snapshot;
}

void ReportIfDue(bool force) noexcept
{
    if (!Config().sample_every) return;
    const auto now = NowNs();
    if (!local.last_report_ns) local.last_report_ns = now;
    if (!force && now - local.last_report_ns < Config().interval_ns) return;
    const auto elapsed_ms = (now - local.last_report_ns) / 1000000;
    local.last_report_ns = now;
    const auto stats = TakeSnapshot();
    // Instrumentation must never interrupt forwarding, even if logging fails.
    try
    {
        const bool has_counts = std::any_of(stats.counters.begin(), stats.counters.end(),
                                           [](uint64_t n) { return n != 0; });
        if (!has_counts) return;
        std::ostringstream out;
        out << "[media-latency] thread=" << std::this_thread::get_id()
            << " window_ms=" << elapsed_ms << " sample_every=" << Config().sample_every;
        for (std::size_t i = 0; i < stats.counters.size(); ++i)
            if (stats.counters[i]) out << ' ' << kCounterNames[i] << '=' << stats.counters[i];
        out << std::fixed << std::setprecision(2);
        for (std::size_t i = 0; i < stats.stages.size(); ++i)
        {
            const auto& h = stats.stages[i];
            if (!h.Count()) continue;
            out << ' ' << kStageNames[i] << "_us={n:" << h.Count()
                << ",avg:" << h.MeanUs() << ",p50_le:" << h.PercentileUs(50)
                << ",p95_le:" << h.PercentileUs(95) << ",p99_le:" << h.PercentileUs(99)
                << ",max:" << h.MaxUs() << '}';
        }
        LOG_INFO(out.str());
    }
    catch (...) {}
}
} // namespace media_latency
