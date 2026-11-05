#include "ProbeController.h"

namespace media
{
ProbeDecision ProbeController::OnProcessInterval(const ProbeContext&)
{
    // TODO(algorithm): 根据需求、RTT、估计和上轮结果决定触发/退避及探测参数。
    // 返回 {true, std::nullopt} 表示接管策略但暂不探测；
    // 返回 {true, ProbePlan{bps, duration_ms, max_bytes}} 表示请求一轮探测。
    return {};
}

void ProbeController::OnFeedback(const TransportFeedback&, const PacketGroupObservation&,
                                 const BweResult&)
{
    // TODO(algorithm): 按 probe_cluster_id 关联反馈，判断排队、丢包和容量。
}

void ProbeController::OnProbeEvent(const ProbeEvent&)
{
    // TODO(algorithm): 记录执行起止/中止；等待足够反馈后再评估探测结果。
}

void ProbeController::Reset()
{
    // TODO(algorithm): 清理探测历史、退避和未完成结果，避免跨路径复用。
}
}
