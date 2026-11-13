#include "PacedSendQueue.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <deque>
#include <mutex>
#include <optional>
#include <utility>

namespace media::transport
{
namespace
{
uint64_t NowMs()
{
    return std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count();
}
// Reserved capacity keeps a video burst from consuming audio/control slots.
constexpr std::array<std::size_t, 3> kByteLimits{16 * 1024, 48 * 1024, 192 * 1024};
constexpr std::array<std::size_t, 3> kPacketLimits{32, 96, 384};
constexpr double kBurstBytes = 1200;
}

struct PacedSendQueue::State
{
    Schedule schedule;
    Consume consume;
    Rate rate;
    Clock clock;
    mutable std::mutex mutex;
    std::array<std::deque<Packet>, 3> queues;
    std::array<std::size_t, 3> bytes{}, packets{};
    uint64_t rejected = 0, last_ms = 0;
    double credit = 0;
    uint32_t previous_rate = 0;
    bool scheduled = false, closed = false;

    void Clear()
    {
        // A popped packet remains charged until its consume callback returns.
        for (std::size_t lane = 0; lane < queues.size(); ++lane)
        {
            for (const auto& packet : queues[lane]) { bytes[lane] -= packet.bytes.size(); --packets[lane]; }
            queues[lane].clear();
        }
        credit = 0;
        previous_rate = 0;
        last_ms = clock();
    }
    bool Empty() const
    { return queues[0].empty() && queues[1].empty() && queues[2].empty(); }
};

PacedSendQueue::PacedSendQueue(Schedule schedule, Consume consume, Rate rate, Clock clock)
    : state_(std::make_shared<State>())
{
    state_->schedule = std::move(schedule);
    state_->consume = std::move(consume);
    state_->rate = std::move(rate);
    state_->clock = clock ? std::move(clock) : Clock{NowMs};
    state_->last_ms = state_->clock();
}

PacedSendQueue::~PacedSendQueue() { Close(); }

PacedSendQueue::Result PacedSendQueue::Send(Priority priority, const uint8_t* data, std::size_t size,
                                          std::function<bool()> valid, std::function<void()> on_sent)
{
    const auto state = state_;
    const auto lane = static_cast<std::size_t>(priority);
    bool wake = false;
    {
        std::lock_guard<std::mutex> lock(state->mutex);
        if (state->closed) return Result::Closed;
        if (!data || !size || lane >= state->queues.size() || size > kByteLimits[lane] - state->bytes[lane] ||
            state->packets[lane] >= kPacketLimits[lane])
        { ++state->rejected; return Result::Full; }
        // Check capacity before copying, so overload cannot accumulate packet tasks.
        try
        {
            state->queues[lane].push_back({priority, {data, data + size}, std::move(valid),
                                          std::move(on_sent), state->clock()});
        }
        catch (const std::bad_alloc&) { ++state->rejected; return Result::Full; }
        state->bytes[lane] += size;
        ++state->packets[lane];
        if (!state->scheduled) { state->scheduled = true; wake = true; }
    }
    if (wake && !Wake(state, 0)) return Result::Closed;
    return Result::Accepted;
}

bool PacedSendQueue::Wake(const std::shared_ptr<State>& state, uint32_t delay_ms)
{
    const std::weak_ptr<State> weak = state;
    bool accepted = false;
    try
    {
        accepted = state->schedule && state->schedule(delay_ms, [weak] {
            if (const auto current = weak.lock()) Pump(current);
        });
    }
    catch (...) {}
    if (!accepted)
    {
        std::lock_guard<std::mutex> lock(state->mutex);
        state->closed = true;
        state->Clear();
    }
    return accepted;
}

void PacedSendQueue::Pump(const std::shared_ptr<State>& state)
{
    const auto rate = state->rate ? state->rate() : 0;
    {
        std::lock_guard<std::mutex> lock(state->mutex);
        if (state->closed) return;
        const auto now = state->clock();
        const auto elapsed = now >= state->last_ms ? now - state->last_ms : 0;
        // Credit accrues at the OLD rate; a rate increase cannot rewrite history.
        // Cap catch-up after an idle/blocked owner, rather than releasing a huge burst.
        state->credit = std::min(kBurstBytes, state->credit +
            static_cast<double>(elapsed) * state->previous_rate / 8000.0);
        state->last_ms = now;
        state->previous_rate = rate;
    }
    unsigned controls = 0;
    for (unsigned batch = 0; batch < 16; ++batch)
    {
        std::optional<Packet> packet;
        std::size_t lane;
        {
            std::lock_guard<std::mutex> lock(state->mutex);
            if (state->closed) return;
            if (controls < 4 && !state->queues[0].empty()) { lane = 0; ++controls; }
            else if (rate && state->credit >= 0 && !state->queues[1].empty()) lane = 1;
            else if (rate && state->credit >= 0 && !state->queues[2].empty()) lane = 2;
            else break;
            packet.emplace(std::move(state->queues[lane].front()));
            state->queues[lane].pop_front();
            if (lane) state->credit -= packet->bytes.size(); // At most one packet of debt.
        }
        const auto size = packet->bytes.size();
        bool failed = false;
        try
        {
            if (!packet->valid || packet->valid()) state->consume(std::move(*packet));
        }
        catch (...) { failed = true; }
        {
            std::lock_guard<std::mutex> lock(state->mutex);
            state->bytes[lane] -= size;
            --state->packets[lane];
            if (failed) { state->closed = true; state->Clear(); return; }
        }
    }
    uint32_t delay = 1;
    {
        std::lock_guard<std::mutex> lock(state->mutex);
        if (state->closed || state->Empty()) { state->scheduled = false; return; }
        if (state->queues[0].empty() && state->credit < 0 && rate)
            delay = static_cast<uint32_t>(std::clamp(std::ceil(-state->credit * 8000.0 / rate), 1.0, 5.0));
        else if (!rate && state->queues[0].empty()) delay = 5;
    }
    Wake(state, delay);
}

void PacedSendQueue::Reset()
{
    std::lock_guard<std::mutex> lock(state_->mutex);
    state_->Clear();
}

void PacedSendQueue::Close()
{
    std::lock_guard<std::mutex> lock(state_->mutex);
    state_->closed = true;
    state_->Clear();
}

PacedSendQueue::Stats PacedSendQueue::GetStats() const
{
    std::lock_guard<std::mutex> lock(state_->mutex);
    Stats result;
    result.rejected_packets = state_->rejected;
    const auto now = state_->clock();
    for (std::size_t lane = 0; lane < state_->queues.size(); ++lane)
    {
        result.queued_bytes += state_->bytes[lane];
        result.queued_packets += state_->packets[lane];
        if (!state_->queues[lane].empty() && now >= state_->queues[lane].front().queued_ms)
            result.oldest_wait_ms = std::max(result.oldest_wait_ms, now - state_->queues[lane].front().queued_ms);
    }
    return result;
}
}
