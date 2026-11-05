#pragma once

#include "BandwidthUsage.h"
#include "NetworkStats.h"

#include <optional>
#include <vector>

namespace media
{
// 一个已完成的观测组。发送/反馈时间属于本机单调时钟；接收时间属于
// 对端时钟。只比较各自时钟内的间隔，不直接相减计算单向时延。
struct PacketGroup
{
    uint64_t id = 0;
    uint64_t feedback_time_ms = 0;
    uint64_t first_send_time_ms = 0;
    uint64_t last_send_time_ms = 0;
    std::optional<uint64_t> first_receive_time_ms;
    std::optional<uint64_t> last_receive_time_ms;
    uint32_t received_packets = 0;
    uint32_t lost_packets = 0;
    uint64_t received_bytes = 0;
    uint64_t lost_bytes = 0;
    std::optional<double> queue_delay_ms;
};

struct PacketGroupContext
{
    uint64_t now_ms = 0;
    uint32_t target_bitrate_bps = 0;
    uint32_t rtt_ms = 0;
};

struct PacketGroupObservation
{
    std::vector<PacketGroup> completed_groups;
    // 未给出的结果继续使用现有逐包估算。0 和“尚无结果”必须区分。
    std::optional<BandwidthUsage> usage;
    std::optional<double> delay_trend_ms;
    std::optional<uint32_t> receive_bitrate_bps;
    std::optional<double> loss_rate;
};

// 一条 Transport 独占一个实例，由 SendSideController 的锁串行调用。
// 这里只做有界计算；不可发包、阻塞或回调外层控制器。
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
    PacketGroupObservation OnFeedback(const TransportFeedback& feedback,
                                      const PacketGroupContext& context) override;
    void Reset() override;
};
}
