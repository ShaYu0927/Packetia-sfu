#ifndef _RTP_RING_BUFFER_H_
#define _RTP_RING_BUFFER_H_

#include <cstdint>
#include <cstddef>
#include <array>
#include <atomic>
#include <utility>

struct RtpWorkJob
{
    std::uint64_t key = 0;
    std::uint32_t type = 0;           // 0=RTP, 1=RTCP
    void*         payload = nullptr;  // Packet*
    std::size_t   payload_len = 0;
    std::uint64_t enqueue_ts = 0;     // 可选：外部填充
};

typedef struct RtpQueueStats
{
    std::uint64_t pushed = 0;
    std::uint64_t popped = 0;
    std::uint64_t dropped = 0;
    std::uint32_t max_depth_seen = 0;
    std::uint16_t rtp = 0;
    std::uint16_t rtcp = 0;
}RtpQueueStats;





enum : std::uint32_t 
{
    RTP_JOB_RTP  = 0,
    RTP_JOB_RTCP = 1,
};

template<typename T, std::size_t CapacityPow2>
class SpscRing
{
public:
    static_assert(CapacityPow2 >= 2, "Capacity must be >= 2");
    static_assert((CapacityPow2 & (CapacityPow2 - 1)) == 0,
                  "CapacityPow2 must be power of two");

    SpscRing() = default;

    SpscRing(const SpscRing&) = delete;
    SpscRing& operator=(const SpscRing&) = delete;

    bool push(T&& item);
    bool pop(T& out);
    std::uint32_t approx_size() const;
    const RtpQueueStats& stats() const;

    void reset_stats();

private:
    static constexpr std::uint32_t mask_ = static_cast<std::uint32_t>(CapacityPow2 - 1);

    std::array<T, CapacityPow2> buf_{};
    std::atomic<std::uint32_t> read_{0};
    std::atomic<std::uint32_t> write_{0};
    RtpQueueStats st_{};

};

#endif /* _RTP_RING_BUFFER_H_ */

template <typename T, std::size_t CapacityPow2>
inline bool SpscRing<T, CapacityPow2>::push(T &&item)
{
    const std::uint32_t w = write_.load(std::memory_order_relaxed);
    const std::uint32_t r = read_.load(std::memory_order_acquire);

    if (((w + 1) & mask_) == (r & mask_))
    {
        // full
        st_.dropped++;
        return false;
    }

    buf_[w & mask_] = std::move(item);
    write_.store(w + 1, std::memory_order_release);

    st_.pushed++;
    const std::uint32_t depth = static_cast<std::uint32_t>(w + 1 - r);
    if (depth > st_.max_depth_seen) st_.max_depth_seen = depth;
    return true;
}

template <typename T, std::size_t CapacityPow2>
inline bool SpscRing<T, CapacityPow2>::pop(T &out)
{
    const std::uint32_t r = read_.load(std::memory_order_relaxed);
    const std::uint32_t w = write_.load(std::memory_order_acquire);

    if (r == w) return false; // empty

    out = std::move(buf_[r & mask_]);
    read_.store(r + 1, std::memory_order_release);
    st_.popped++;
    return true;
}

template <typename T, std::size_t CapacityPow2>
inline std::uint32_t SpscRing<T, CapacityPow2>::approx_size() const
{
    const std::uint32_t r = read_.load(std::memory_order_acquire);
    const std::uint32_t w = write_.load(std::memory_order_acquire);
    return static_cast<std::uint32_t>(w - r);
}

template <typename T, std::size_t CapacityPow2>
inline const RtpQueueStats &SpscRing<T, CapacityPow2>::stats() const
{
    return st_;
}

template <typename T, std::size_t CapacityPow2>
inline void SpscRing<T, CapacityPow2>::reset_stats()
{
    st_ = RtpQueueStats{};
}



/*
 * RTP 专用双队列：
 * - RTCP 高优先级队列（建议容量小一些）
 * - RTP  普通队列（容量大一些）
 *
 * 消费顺序：
 *   try_pop(): 先 RTCP 后 RTP
 *
 * 上层建议：
 * - IO/生产者线程根据 is_rtcp 决定 push 到哪个队列
 * - Worker/消费者线程循环 try_pop
 */
template<std::size_t RtpCapPow2, std::size_t RtcpCapPow2>
class RtpRingBuffer
{
public:
    struct Stats
    {
        typename SpscRing<RtpWorkJob, RtpCapPow2>::Stats  rtp;
        typename SpscRing<RtpWorkJob, RtcpCapPow2>::Stats rtcp;
    };

    // push RTP：满时返回 false（上层负责 drop + release Packet）
    bool push_rtp(RtpWorkJob&& job)
    {
        job.type = RTP_JOB_RTP;
        return rtp_.push(std::move(job));
    }

    // push RTCP：满时返回 false（上层负责 drop + release Packet）
    bool push_rtcp(RtpWorkJob&& job)
    {
        job.type = RTP_JOB_RTCP;
        return rtcp_.push(std::move(job));
    }

    // 优先弹出 RTCP，其次 RTP
    bool try_pop(RtpWorkJob& out)
    {
        if (rtcp_.pop(out)) return true;
        return rtp_.pop(out);
    }

    std::uint32_t approx_rtp_size() const { return rtp_.approx_size(); }
    std::uint32_t approx_rtcp_size() const { return rtcp_.approx_size(); }

    RtpQueueStats stats() const
    {
        RtpQueueStats s;
        s.rtp  = rtp_.stats();
        s.rtcp = rtcp_.stats();
        return s;
    }

    void reset_stats()
    {
        rtp_.reset_stats();
        rtcp_.reset_stats();
    }

private:
    SpscRing<RtpWorkJob, RtpCapPow2>  rtp_;
    SpscRing<RtpWorkJob, RtcpCapPow2> rtcp_;
};

