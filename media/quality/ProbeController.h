#pragma once

#include "bwe/PacketGroupObserver.h"

#include <optional>

namespace media
{
struct ProbeRequest
{
    uint64_t now_ms = 0;
    uint64_t path_epoch = 0;
    uint64_t active_bitrate_bps = 0;
    uint32_t candidate_bitrate_bps = 0;
};

struct ProbeContext
{
    ProbeRequest request;
    BitrateConstraints constraints;
    uint32_t target_bitrate_bps = 0;
    uint32_t rtt_ms = 0;
    std::optional<BweResult> bandwidth_estimate;
};

struct ProbePlan
{
    // 算法填写下面三项；均为额外 padding 的预算，不包含活动媒体。
    uint32_t bitrate_bps = 0;
    uint32_t duration_ms = 0;
    uint64_t max_bytes = 0;
    // 由框架填写。编号在 Transport 生命周期内不重复，Reset 不回绕。
    int cluster_id = -1;
    uint64_t generation = 0;
};

struct ProbeDecision
{
    // false: 使用现有受限恢复探测。true + 空 plan: 算法要求本次不探测。
    bool handled = false;
    std::optional<ProbePlan> plan;
};

enum class ProbeEventType { Started, Finished, Aborted };

struct ProbeEvent
{
    ProbeEventType type = ProbeEventType::Started;
    ProbePlan plan;
    uint64_t path_epoch = 0;
    uint64_t at_time_ms = 0;
    // 与执行预算一致，包含保守的头部/加密开销；逐包实际大小在反馈中。
    uint64_t accounted_bytes = 0;
};

// Transport 独占，方法在 SendSideController 锁内执行，不允许外部回调或 IO。
// 执行完成不代表成功测出容量；算法应关联反馈里的 probe_cluster_id 再判断。
class IProbeController
{
public:
    virtual ~IProbeController() = default;
    virtual ProbeDecision OnProcessInterval(const ProbeContext& context) = 0;
    virtual void OnFeedback(const TransportFeedback& feedback,
                            const PacketGroupObservation& groups, const BweResult& estimate) = 0;
    virtual void OnProbeEvent(const ProbeEvent& event) = 0;
    virtual void Reset() = 0;
};

class ProbeController final : public IProbeController
{
public:
    ProbeDecision OnProcessInterval(const ProbeContext& context) override;
    void OnFeedback(const TransportFeedback& feedback,
                    const PacketGroupObservation& groups, const BweResult& estimate) override;
    void OnProbeEvent(const ProbeEvent& event) override;
    void Reset() override;
};
}
