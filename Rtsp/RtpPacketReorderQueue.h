#pragma once

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <map>
#include <stdexcept>
#include <type_traits>
#include <utility>

// One queue per SSRC, owned by its media worker. Callbacks run synchronously
// and must not re-enter the queue. No playback pacing or codec frame assembly
// is performed here. Sequence windows must be smaller than half an RTP cycle.
template<typename Packet, typename Seq = uint16_t>
class RtpPacketReorderQueue
{
    static_assert(std::is_same_v<Seq, uint16_t>, "RTP uses 16-bit sequence numbers");
public:
    using Clock = std::chrono::steady_clock;
    using TimePoint = Clock::time_point;
    using Callback = std::function<void(Seq, Packet)>;
    struct ResyncConfig
    {
        uint16_t consecutive_packets = 3; // 0 disables forward resynchronization.
        uint32_t stalled_ms = 500; // No normal output for at least this long.
        uint32_t probation_ms = 100; // Total time to observe a consecutive run.
    };
    struct Stats
    {
        size_t emitted = 0;
        size_t lost = 0; // Missing sequences skipped, not packets discarded.
        size_t duplicates = 0; // Duplicates still held in the queue.
        size_t late = 0; // Includes repeats of already emitted packets.
        size_t out_of_window = 0;
        size_t timeouts = 0;
        size_t overflows = 0;
        size_t resync_attempts = 0;
        size_t resyncs = 0;
    };

    explicit RtpPacketReorderQueue(uint16_t max_gap = 1000, size_t max_cache = 50,
                                   uint32_t flush_timeout_ms = 100)
        : RtpPacketReorderQueue(max_gap, max_cache, flush_timeout_ms, ResyncConfig{}) {}

    RtpPacketReorderQueue(uint16_t max_gap, size_t max_cache, uint32_t flush_timeout_ms,
                         ResyncConfig resync)
        : max_gap_(max_gap), max_cache_(max_cache), timeout_(flush_timeout_ms), resync_(resync)
    {
        if (!max_gap_ || max_gap_ > 0x8000 || max_cache_ >= 0x8000)
            throw std::invalid_argument("RTP reorder window must be within half a sequence cycle");
        if (resync_.consecutive_packets && (resync_.consecutive_packets < 2 ||
            resync_.consecutive_packets >= 0x8000 || !resync_.stalled_ms || !resync_.probation_ms))
            throw std::invalid_argument("RTP resync needs a bounded consecutive run and positive time limits");
    }

    void setOnPacketSorted(Callback callback) { callback_ = std::move(callback); }

    void inputPacket(Seq seq, Packet packet, TimePoint now = Clock::now())
    {
        if (!started_) {
            started_ = true;
            next_ = seq;
            Emit(next_, std::move(packet), now);
            return;
        }
        flushExpired(now);
        const auto delta = static_cast<uint16_t>(seq - static_cast<uint16_t>(next_));
        if (delta == 0x8000) { ClearResync(); ++stats_.out_of_window; return; }
        if (delta > 0x8000) { ++stats_.late; return; }
        if (delta >= max_gap_) {
            if (ConfirmForwardResync(seq, now)) {
                ++stats_.resyncs;
                stats_.lost += delta;
                Emit(next_ + delta, std::move(packet), now);
            } else {
                ++stats_.out_of_window;
            }
            return;
        }
        ClearResync(); // Normal traffic always wins over an unconfirmed jump.
        if (!delta) {
            Emit(next_, std::move(packet), now);
            flushBuffered(now);
            return;
        }
        const int64_t extended = next_ + delta;
        const auto inserted = buffer_.emplace(extended, Buffered{std::move(packet), now});
        if (!inserted.second) { ++stats_.duplicates; return; }
        if (buffer_.size() > max_cache_) {
            ++stats_.overflows;
            SkipToFirst();
            flushBuffered(now);
        }
        // A zero timeout means no reordering wait, including the first gap.
        flushExpired(now);
    }

    void flushBuffered(TimePoint now = Clock::now())
    {
        while (!buffer_.empty() && buffer_.begin()->first == next_) {
            auto node = buffer_.extract(buffer_.begin());
            Emit(node.key(), std::move(node.mapped().packet), now);
        }
    }

    // Owner must also call this from a periodic tick when RTP stops arriving.
    // Deadlines belong to the blocked packets; new arrivals never extend them.
    void flushExpired(TimePoint now = Clock::now())
    {
        if (resync_count_ && (now < resync_started_ ||
            now - resync_started_ >= std::chrono::milliseconds(resync_.probation_ms))) ClearResync();
        while (!buffer_.empty()) {
            auto oldest = buffer_.begin()->second.arrived;
            for (const auto& entry : buffer_)
                if (entry.second.arrived < oldest) oldest = entry.second.arrived;
            if (now < oldest || now - oldest < timeout_) break;
            ++stats_.timeouts;
            SkipToFirst();
            flushBuffered(now);
        }
    }

    // Explicit drain: skip all remaining gaps, independently of deadlines.
    void flushTimeout(TimePoint now = Clock::now())
    {
        ClearResync();
        while (!buffer_.empty()) { SkipToFirst(); flushBuffered(now); }
    }

    void reset()
    {
        buffer_.clear();
        started_ = false;
        next_ = 0;
        last_progress_ = {};
        ClearResync();
        stats_ = {};
    }
    size_t getLostCount() const { return stats_.lost; }
    size_t getDropCount() const { return stats_.duplicates + stats_.late + stats_.out_of_window; }
    size_t getBufferedCount() const { return buffer_.size(); }
    const Stats& getReorderStats() const { return stats_; }

private:
    struct Buffered { Packet packet; TimePoint arrived; };
    void ClearResync() { resync_count_ = 0; }
    bool ConfirmForwardResync(Seq seq, TimePoint now)
    {
        // Caller must validate the RTP source and payload before admission.
        // Never reinterpret late/backward or half-cycle-ambiguous packets.
        if (!resync_.consecutive_packets || !buffer_.empty() || now < last_progress_ ||
            now - last_progress_ < std::chrono::milliseconds(resync_.stalled_ms)) {
            ClearResync();
            return false;
        }
        if (resync_count_ && seq == resync_last_) return false; // No deadline extension.
        if (!resync_count_ || seq != static_cast<Seq>(resync_last_ + 1)) {
            resync_count_ = 1;
            resync_started_ = now;
            ++stats_.resync_attempts;
        } else {
            ++resync_count_;
        }
        resync_last_ = seq;
        return resync_count_ >= resync_.consecutive_packets;
    }
    void SkipToFirst()
    {
        stats_.lost += static_cast<size_t>(buffer_.begin()->first - next_);
        next_ = buffer_.begin()->first;
    }
    void Emit(int64_t extended, Packet packet, TimePoint now)
    {
        next_ = extended + 1;
        last_progress_ = now;
        ClearResync();
        ++stats_.emitted;
        if (callback_) callback_(static_cast<Seq>(extended), std::move(packet));
    }

    uint16_t max_gap_;
    size_t max_cache_;
    std::chrono::milliseconds timeout_;
    ResyncConfig resync_;
    TimePoint last_progress_{}, resync_started_{};
    Seq resync_last_ = 0;
    uint16_t resync_count_ = 0;
    bool started_ = false;
    int64_t next_ = 0;
    std::map<int64_t, Buffered> buffer_;
    Callback callback_;
    Stats stats_;
};
