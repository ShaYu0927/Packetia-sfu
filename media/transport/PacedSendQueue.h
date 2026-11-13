#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
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
        std::size_t queued_bytes = 0, queued_packets = 0;
        uint64_t oldest_wait_ms = 0, rejected_packets = 0;
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
