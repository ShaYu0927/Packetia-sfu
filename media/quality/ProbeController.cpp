#include "ProbeController.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <utility>

namespace media
{
namespace
{
constexpr uint64_t kEstimateFreshnessMs = 1000;
constexpr uint32_t kProbeDurationMs = 500;
constexpr uint64_t kMaxProbeBytes = 128 * 1024;
constexpr uint64_t kMinProbeBytes = 1500;
constexpr size_t kMinFeedbackPackets = 5;
constexpr uint64_t kMinSendSpanMs = 100;
constexpr double kMaxLossRate = 0.05;
constexpr uint64_t kProbeOveragePercent = 150;

bool IsCongested(const BweResult& estimate)
{
    return estimate.usage == BandwidthUsage::kOverUsing ||
        !std::isfinite(estimate.loss_rate) || estimate.loss_rate > kMaxLossRate;
}
}

const char* ToString(ProbeDecisionReason reason)
{
    switch (reason)
    {
    case ProbeDecisionReason::Planned: return "Planned";
    case ProbeDecisionReason::InProgress: return "InProgress";
    case ProbeDecisionReason::CoolingDown: return "CoolingDown";
    case ProbeDecisionReason::Congested: return "Congested";
    case ProbeDecisionReason::NoDemand: return "NoDemand";
    case ProbeDecisionReason::NoHeadroom: return "NoHeadroom";
    case ProbeDecisionReason::RecoveryExceedsMaximum: return "RecoveryExceedsMaximum";
    case ProbeDecisionReason::TooSmall: return "TooSmall";
    case ProbeDecisionReason::NetworkUnavailable: return "NetworkUnavailable";
    case ProbeDecisionReason::StalePath: return "StalePath";
    default: return "Unspecified";
    }
}

ProbeDecision ProbeController::OnProcessInterval(const ProbeContext& context)
{
    OnTime(context.request.now_ms);
    const auto reason = CheckStartConditions(context);
    if (reason != ProbeDecisionReason::Planned) return {true, std::nullopt, reason};

    auto decision = BuildPlan(context);
    if (decision.plan)
        feedback_wait_ms_ = static_cast<uint32_t>(std::clamp<uint64_t>(uint64_t(context.rtt_ms) * 2 + 500, 1000, 3000));
    return decision;
}

ProbeDecisionReason ProbeController::CheckStartConditions(const ProbeContext& context) const
{
    const auto& request = context.request;
    if (cluster_) return ProbeDecisionReason::InProgress;
    if (last_completed_ms_ && (request.now_ms < *last_completed_ms_ ||
        request.now_ms - *last_completed_ms_ < cooldown_ms_)) return ProbeDecisionReason::CoolingDown;
    if (!request.candidate_bitrate_bps) return ProbeDecisionReason::NoDemand;
    if (request.required_budget_bps && *request.required_budget_bps > context.constraints.max_bitrate_bps)
        return ProbeDecisionReason::RecoveryExceedsMaximum;
    if (request.active_bitrate_bps >= context.constraints.max_bitrate_bps)
        return ProbeDecisionReason::NoHeadroom;

    // Old congestion must not block recovery forever when paused video is silent.
    const bool fresh_estimate = context.bandwidth_estimate && context.bandwidth_estimate_time_ms &&
        request.now_ms >= *context.bandwidth_estimate_time_ms &&
        request.now_ms - *context.bandwidth_estimate_time_ms <= kEstimateFreshnessMs;
    if (fresh_estimate && IsCongested(*context.bandwidth_estimate)) return ProbeDecisionReason::Congested;
    return ProbeDecisionReason::Planned;
}

ProbeDecision ProbeController::BuildPlan(const ProbeContext& context) const
{
    const auto& request = context.request;
    const uint64_t target = context.target_bitrate_bps ? context.target_bitrate_bps :
        context.constraints.start_bitrate_bps;
    const uint64_t ceiling = std::min<uint64_t>(context.constraints.max_bitrate_bps,
        std::max(target * 3 / 2, target + 64000));
    const uint64_t recovery_target = request.required_budget_bps.value_or(
        request.active_bitrate_bps + request.candidate_bitrate_bps);
    // The allocation threshold is a desired measured budget. Probe above it to
    // leave room for measurement headroom and conservative padding accounting.
    const uint64_t desired = (recovery_target * kProbeOveragePercent + 99) / 100;
    const uint64_t total = std::min(ceiling, desired);
    if (total <= request.active_bitrate_bps) return {true, std::nullopt, ProbeDecisionReason::NoHeadroom};

    ProbePlan plan;
    plan.bitrate_bps = static_cast<uint32_t>(total - request.active_bitrate_bps);
    plan.duration_ms = kProbeDurationMs;
    plan.max_bytes = std::min<uint64_t>(kMaxProbeBytes, uint64_t(plan.bitrate_bps) * plan.duration_ms / 8000);
    plan.pacing_bitrate_bps = static_cast<uint32_t>(total);
    // Tiny clusters cannot provide a useful time/rate measurement.
    if (plan.max_bytes < kMinProbeBytes) return {true, std::nullopt, ProbeDecisionReason::TooSmall};
    return {true, plan, ProbeDecisionReason::Planned};
}

void ProbeController::OnProbeEvent(const ProbeEvent& event)
{
    if (event.type == ProbeEventType::Started)
    {
        if (cluster_) return;
        cluster_.emplace();
        cluster_->plan = event.plan;
        cluster_->started_ms = event.at_time_ms;
        cluster_->feedback_wait_ms = feedback_wait_ms_;
        return;
    }
    if (!cluster_ || cluster_->plan.cluster_id != event.plan.cluster_id ||
        cluster_->plan.generation != event.plan.generation) return;
    if (event.type == ProbeEventType::Aborted || event.at_time_ms < cluster_->started_ms)
        Complete(ProbeOutcome::Aborted, event.at_time_ms);
    else
    {
        cluster_->finished_ms = event.at_time_ms;
        cluster_->accounted_bytes = event.accounted_bytes;
        // Previously received TWCC may already cover the cluster.
        Evaluate(event.at_time_ms);
    }
}

void ProbeController::OnFeedback(const TransportFeedback& feedback, const PacketGroupObservation&,
                                  const BweResult& estimate)
{
    OnTime(feedback.feedback_time_ms);
    if (!cluster_ || feedback.feedback_time_ms < cluster_->started_ms) return;
    bool has_current_packet = false;
    for (const auto& packet : feedback.packet_feedbacks)
    {
        const auto& sent = packet.sent_packet;
        if (sent.transport_sequence < 0 || !sent.size_bytes || sent.send_time_ms < cluster_->started_ms ||
            sent.send_time_ms - cluster_->started_ms >= cluster_->plan.duration_ms ||
            sent.send_time_ms > feedback.feedback_time_ms) continue;
        if (sent.is_probe && sent.probe_cluster_id != cluster_->plan.cluster_id) continue;
        has_current_packet = true;
        // Bound memory even if a peer supplies many fragmented or repeated reports.
        if (cluster_->packets.size() < 4096) cluster_->packets.emplace(sent.transport_sequence, packet);
    }
    // Media and padding share this transport. Media-only feedback can also stop a probe.
    if (has_current_packet && (IsCongested(estimate) || feedback.LossRate() > kMaxLossRate))
    {
        Complete(ProbeOutcome::Congested, feedback.feedback_time_ms);
        return;
    }
    Evaluate(feedback.feedback_time_ms);
}

void ProbeController::Evaluate(uint64_t now_ms)
{
    if (!cluster_) return;
    const auto& cluster = *cluster_;
    size_t probes = 0, lost = 0;
    uint64_t first = std::numeric_limits<uint64_t>::max(), last = 0;
    uint64_t probe_bytes = 0;
    uint64_t first_probe_receive = std::numeric_limits<uint64_t>::max(), last_probe_receive = 0;
    for (const auto& entry : cluster.packets)
    {
        const auto& packet = entry.second;
        if (!packet.sent_packet.is_probe) continue;
        ++probes;
        if (!packet.received) { ++lost; continue; }
        first = std::min(first, packet.sent_packet.send_time_ms);
        last = std::max(last, packet.sent_packet.send_time_ms);
        probe_bytes += packet.sent_packet.size_bytes;
        first_probe_receive = std::min(first_probe_receive, packet.receive_time_ms);
        last_probe_receive = std::max(last_probe_receive, packet.receive_time_ms);
    }
    if (probes < kMinFeedbackPackets) return;
    if (lost * 20 > probes)
    { Complete(ProbeOutcome::Congested, now_ms); return; }

    // Stop on sustained queue growth during execution, before waiting for Finished.
    if (last > first && last - first >= kMinSendSpanMs && last_probe_receive >= first_probe_receive)
    {
        const uint64_t send_span = last - first;
        const uint64_t receive_span = last_probe_receive - first_probe_receive;
        if (receive_span > send_span + std::max<uint64_t>(20, send_span / 5))
        { Complete(ProbeOutcome::Congested, now_ms); return; }
    }
    if (!cluster.finished_ms) return;
    // The executor charges 300 bytes per padding packet. Unknown feedback is not a receipt.
    const uint64_t expected_packets = cluster.accounted_bytes / 300;
    if (expected_packets < kMinFeedbackPackets || probes * 5 < expected_packets * 4) return;
    const uint64_t execution_ms = std::min<uint64_t>(cluster.plan.duration_ms,
        *cluster.finished_ms - cluster.started_ms);
    if (last <= first || last - first < std::max<uint64_t>(kMinSendSpanMs, execution_ms * 7 / 10)) return;
    const uint64_t send_span = last - first;
    if (probe_bytes * 8000.0 / send_span < cluster.plan.bitrate_bps * 0.7)
    { Complete(ProbeOutcome::Inconclusive, now_ms); return; }

    uint64_t bytes = 0, first_send_size = 0, first_receive_size = 0;
    uint64_t receive_first = std::numeric_limits<uint64_t>::max(), receive_last = 0;
    for (const auto& entry : cluster.packets)
    {
        const auto& packet = entry.second;
        const auto& sent = packet.sent_packet;
        if (!packet.received || sent.send_time_ms < first || sent.send_time_ms > last) continue;
        bytes += sent.size_bytes;
        if (sent.send_time_ms == first && !first_send_size) first_send_size = sent.size_bytes;
        if (packet.receive_time_ms < receive_first)
        { receive_first = packet.receive_time_ms; first_receive_size = sent.size_bytes; }
        receive_last = std::max(receive_last, packet.receive_time_ms);
    }
    if (receive_last <= receive_first) return;
    const uint64_t receive_span = receive_last - receive_first;
    if (receive_span > send_span + std::max<uint64_t>(20, send_span / 5))
    { Complete(ProbeOutcome::Congested, now_ms); return; }
    // Use actual wire-send/arrival spreads, not enqueue time or planned bitrate.
    // Subtract each interval's first packet to avoid the small-cluster rate bias.
    const double send_rate = (bytes - first_send_size) * 8000.0 / send_span;
    const double receive_rate = (bytes - first_receive_size) * 8000.0 / receive_span;
    const double measured = std::min(send_rate, receive_rate) * 0.9;
    const uint32_t ceiling = cluster.plan.pacing_bitrate_bps ? cluster.plan.pacing_bitrate_bps : cluster.plan.bitrate_bps;
    Complete(ProbeOutcome::Succeeded, now_ms, static_cast<uint32_t>(std::min<double>(measured, ceiling)));
}

void ProbeController::Complete(ProbeOutcome outcome, uint64_t now_ms, uint32_t capacity_bps)
{
    result_ = ProbeResult{cluster_->plan.cluster_id, cluster_->plan.generation, outcome, capacity_bps, now_ms};
    cluster_.reset();
    last_completed_ms_ = now_ms;
    cooldown_ms_ = outcome == ProbeOutcome::Succeeded ? 3000 : std::min<uint32_t>(cooldown_ms_ * 2, 30000);
}

void ProbeController::OnTime(uint64_t now_ms)
{
    if (!cluster_) return;
    if (now_ms < cluster_->started_ms)
    { Complete(ProbeOutcome::Aborted, now_ms); return; }
    const uint64_t lifetime = uint64_t(cluster_->plan.duration_ms) + cluster_->feedback_wait_ms;
    if (now_ms - cluster_->started_ms >= lifetime) Complete(ProbeOutcome::TimedOut, now_ms);
}

std::optional<ProbeResult> ProbeController::TakeResult()
{
    return std::exchange(result_, std::nullopt);
}

void ProbeController::Reset()
{
    cluster_.reset();
    result_.reset();
    last_completed_ms_.reset();
    cooldown_ms_ = 3000;
    feedback_wait_ms_ = 1000;
}
}
