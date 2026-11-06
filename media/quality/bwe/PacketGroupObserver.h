#pragma once

#include "BandwidthUsage.h"
#include "NetworkStats.h"

#include <array>
#include <cstddef>
#include <optional>
#include <vector>

namespace media
{
struct PacketGroup
{
    uint64_t id = 0;                               /* 分组标识。 */
    uint64_t feedback_time_ms = 0;                 /* 反馈时间，单位：毫秒。 */
    uint64_t first_send_time_ms = 0;               /* 本组首个数据包的发送时间，单位：毫秒。 */
    uint64_t last_send_time_ms = 0;                /* 本组最后一个数据包的发送时间，单位：毫秒。 */
    std::optional<uint64_t> first_receive_time_ms; /* 最早接收时间，单位：毫秒；无有效时间时为空。 */
    std::optional<uint64_t> last_receive_time_ms;  /* 最晚接收时间，单位：毫秒；无有效时间时为空。 */
    uint32_t received_packets = 0;                 /* 确认已接收的数据包数量。 */
    uint32_t lost_packets = 0;                     /* 反馈中判定丢失的数据包数量。 */
    uint64_t received_bytes = 0;                   /* 已接收数据包的累计字节数。 */
    uint64_t lost_bytes = 0;                       /* 丢失数据包的累计字节数。 */
    std::optional<double> queue_delay_ms;         /* 估算的排队时延，单位：毫秒；无法估算时为空。 */
};

struct PacketGroupContext
{
    uint64_t now_ms = 0;             /* 当前时间，单位：毫秒。 */
    uint32_t target_bitrate_bps = 0; /* 目标发送码率，单位：比特/秒。 */
    uint32_t rtt_ms = 0;             /* 往返时延，单位：毫秒。 */
};

struct PacketGroupObservation
{
    std::vector<PacketGroup> completed_groups;    /* 已完成统计的数据包分组。 */
    std::optional<BandwidthUsage> usage;          /* 带宽使用状态；无有效判定时为空。 */
    std::optional<double> delay_trend_ms;         /* 时延变化趋势，单位：毫秒；无有效估计时为空。 */
    std::optional<uint32_t> receive_bitrate_bps;  /* 接收码率，单位：比特/秒；无有效估计时为空。 */
    std::optional<double> loss_rate;              /* 丢包率，通常取值为 0～1；无有效统计时为空。 */
};

class IPacketGroupObserver
{
public:
    virtual ~IPacketGroupObserver() = default;
    virtual PacketGroupObservation OnFeedback(const TransportFeedback& feedback,
                                              const PacketGroupContext& context) = 0;
    virtual void Reset() = 0;
};

class PacketGroupObserver final : public IPacketGroupObserver
{
public:
    PacketGroupObservation OnFeedback(const TransportFeedback& feedback, const PacketGroupContext& context) override;
    void Reset() override;

private:
    std::vector<const PacketFeedback*> SortAndDeduplicate(const TransportFeedback& feedback);

    static constexpr std::size_t kDedupWindowSize = 4096;
    // 保存扩展序号 + 1，0 表示空槽；窗口外及无有效序号的反馈不再处理。
    std::array<uint64_t, kDedupWindowSize> seen_sequences_{};
    int64_t newest_sequence_ = -1;
    std::optional<PacketGroup> pending_group_;
    std::optional<PacketGroup> previous_group_;
    std::vector<double> trend_history_;
};
}
