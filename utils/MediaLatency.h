#ifndef PACKETIA_UTILS_MEDIALATENCY_H_
#define PACKETIA_UTILS_MEDIALATENCY_H_

#include <array>
#include <cstddef>
#include <cstdint>

// Media forwarding instrumentation, currently connected to the RTP path.
// All timestamps use steady_clock; never mix them with RTP timestamps or
// receive_time_ms from a peer/API.
namespace media_latency
{
uint64_t NowNs() noexcept;

struct PacketTrace
{
    uint64_t ingress_ns = 0;
    uint64_t enqueue_ns = 0;
    uint64_t worker_ns = 0;
    explicit operator bool() const noexcept { return ingress_ns != 0; }
};

struct SendTrace
{
    PacketTrace packet;
    uint64_t submitted_ns = 0;
    explicit operator bool() const noexcept { return submitted_ns != 0; }
};

enum class Stage : std::size_t
{
    Ingress, WorkerQueue, BeforeForward, Fanout,
    UdpSend, UdpTotal, TcpSend, TcpTotal, Count
};

enum class Counter : std::size_t
{
    IngressSampled, WorkerStarted, QueueDropped, ForwardInputs, NoSubscribers,
    SenderAttempts, SenderRejected, UdpSent, UdpFailed, TcpSent, TcpAbandoned, Count
};

// Fixed storage: exact count/mean/max, approximate percentile upper bounds.
// Buckets are exact through 16 us, then have eight subdivisions per doubling.
class Histogram
{
public:
    void Add(uint64_t ns) noexcept;
    uint64_t Count() const noexcept { return count_; }
    double MeanUs() const noexcept;
    double MaxUs() const noexcept { return static_cast<double>(max_ns_) / 1000.0; }
    uint64_t PercentileUs(unsigned percentile) const noexcept;

private:
    static constexpr std::size_t kBuckets = 258;
    std::array<uint64_t, kBuckets> buckets_{};
    uint64_t count_ = 0;
    long double sum_ns_ = 0;
    uint64_t max_ns_ = 0;
};

struct Snapshot
{
    std::array<Histogram, static_cast<std::size_t>(Stage::Count)> stages{};
    std::array<uint64_t, static_cast<std::size_t>(Counter::Count)> counters{};
};

// Sampling is selected once at ingress, so every subscriber of a sampled
// input carries the same trace. Config is read once from the environment.
PacketTrace BeginPacket() noexcept;
void Count(Counter counter) noexcept;
void Observe(Stage stage, uint64_t start_ns, uint64_t end_ns) noexcept;
void WorkerStarted(PacketTrace& packet) noexcept;
void SocketSent(const SendTrace& trace, bool tcp, uint64_t completed_ns) noexcept;

// Synchronous call-chain context. Never capture these TLS values by reference
// across threads: WorkJob, UDP Invoke and TCP queued entries carry value copies.
PacketTrace CurrentPacket() noexcept;
SendTrace CurrentSend() noexcept;
class PacketScope
{
public:
    explicit PacketScope(PacketTrace packet) noexcept;
    ~PacketScope();
    PacketScope(const PacketScope&) = delete;
    PacketScope& operator=(const PacketScope&) = delete;
private:
    PacketTrace previous_packet_;
    SendTrace previous_send_;
};

class SendScope
{
public:
    explicit SendScope(bool retransmit) noexcept;
    ~SendScope();
    SendScope(const SendScope&) = delete;
    SendScope& operator=(const SendScope&) = delete;
private:
    SendTrace previous_;
};

// Thread-local aggregation: no shared lock or per-packet allocation. Called
// from worker/IO loop ticks, outside socket and queue locks, including on exit.
void ReportIfDue(bool force = false) noexcept;
Snapshot TakeSnapshot() noexcept;
} // namespace media_latency

#endif // PACKETIA_UTILS_MEDIALATENCY_H_
