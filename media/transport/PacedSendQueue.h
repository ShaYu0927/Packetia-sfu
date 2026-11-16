#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <vector>

namespace media::transport
{
// One queue per downstream transport, BEFORE cross-thread packet dispatch.
// Schedule must enqueue asynchronous, serial callbacks on the session owner.
class PacedSendQueue
{
public:
    enum class Priority : std::size_t { Control, Audio, Media };
    enum class Result { Accepted, Full, Closed };
    struct Packet
    {
        Priority priority;
        std::vector<uint8_t> bytes;
        std::function<bool()> valid;
        std::function<void()> on_sent;
        uint64_t queued_ms;
    };
    struct Stats
    {
        // 容量占用包含正在执行 consume 回调的包，回调返回后释放。
        std::size_t queued_bytes = 0;
        std::size_t queued_packets = 0;
        uint64_t oldest_wait_ms = 0;
        uint64_t rejected_packets = 0;

        // 等待发送的音频、视频 RTP 字节数。
        std::size_t queued_rtp_bytes = 0;

        // 当前 pacing 发送速率，单位 bit/s。
        uint32_t pacing_bitrate_bps = 0;

        // 按当前速率估算 RTP 排空时间（向上取整）；有积压但速率为零时无值。
        // 不含 pacing 债务、后续入队及 socket 排队时间，不代表端到端时延。
        std::optional<uint64_t> estimated_drain_ms;
    };
    using Schedule = std::function<bool(uint32_t delay_ms, std::function<void()>)>;
    using Consume = std::function<void(Packet)>;
    using Rate = std::function<uint32_t()>; // RTP bytes per second * 8; zero pauses RTP.
    using Clock = std::function<uint64_t()>;

    PacedSendQueue(Schedule schedule, Consume consume, Rate rate, Clock clock = {});
    ~PacedSendQueue();
    PacedSendQueue(const PacedSendQueue&) = delete;
    PacedSendQueue& operator=(const PacedSendQueue&) = delete;
    Result Send(Priority priority, const uint8_t* data, std::size_t size,
                std::function<bool()> valid = {}, std::function<void()> on_sent = {});
    // Cancel queued packets on path changes; an existing wakeup is reused.
    void Reset();
    void Close();
    Stats GetStats() const;

private:
    struct State;
    static bool Wake(const std::shared_ptr<State>& state, uint32_t delay_ms);
    static void Pump(const std::shared_ptr<State>& state);
    std::shared_ptr<State> state_;
};
}
