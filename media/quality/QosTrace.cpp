#include "QosTrace.h"
#include "logger.h"
#include <cstdlib>
#include <string_view>

namespace media
{
namespace
{
const char* UsageName(BandwidthUsage usage)
{
    switch (usage)
    {
        case BandwidthUsage::kNormal: return "normal";
        case BandwidthUsage::kUnderUsing: return "underusing";
        case BandwidthUsage::kOverUsing: return "overusing";
    }
    return "unknown";
}
const char* OutcomeName(ProbeOutcome outcome)
{
    switch (outcome)
    {
        case ProbeOutcome::Succeeded: return "succeeded";
        case ProbeOutcome::Inconclusive: return "inconclusive";
        case ProbeOutcome::Congested: return "congested";
        case ProbeOutcome::TimedOut: return "timed_out";
        case ProbeOutcome::Aborted: return "aborted";
    }
    return "unknown";
}
std::optional<uint64_t> Age(uint64_t now, std::optional<uint64_t> at)
{
    return at && now >= *at ? std::optional<uint64_t>(now - *at) : std::nullopt;
}
packetia::logging::LogFields ControlFields(uint64_t id, const TransportDiagnostics& snapshot)
{
    const auto& input = snapshot.controller;
    const auto& update = input.control;
    return {{"transport_id", id}, {"monotonic_ms", snapshot.at_time_ms}, {"path_epoch", snapshot.path_epoch},
        {"network_available", snapshot.network_available}, {"feedback_valid", input.feedback_valid},
        {"feedback_source", input.feedback_source}, {"feedback_age_ms", Age(snapshot.at_time_ms, input.feedback_time_ms)},
        {"twcc_age_ms", Age(snapshot.at_time_ms, input.twcc_time_ms)},
        {"bwe_estimate_valid", input.bwe_estimate_valid},
        {"usage", input.usage ? UsageName(*input.usage) : nullptr},
        {"receive_bitrate_bps", input.receive_bitrate_bps}, {"loss_rate", input.loss_rate},
        {"smoothed_rtt_ms", input.rtt_ms}, {"smoothed_jitter_ms", input.jitter_ms},
        {"delay_trend_ms", input.delay_trend_ms}, {"delay_slope", input.delay_slope},
        {"target_bitrate_bps", update.has_target_rate ? std::optional<uint32_t>(update.target_rate.target_bitrate_bps) : std::nullopt},
        {"normal_pacing_bps", update.has_pacer_config ? std::optional<uint32_t>(update.pacer_config.pacing_bitrate_bps) : std::nullopt}};
}
}

bool QosTraceEnabled() noexcept
{
    static const bool enabled = [] {
        const char* value = std::getenv("PACKETIA_QOS_LOG");
        return value && (std::string_view(value) == "1" || std::string_view(value) == "true");
    }();
    if (!enabled) return false;
    try { return GlobalLogger().min_level() <= BaseLogger::INFO; }
    catch (...) { return false; }
}

void WriteQosEvent(const char* event, const packetia::logging::LogFields& fields) noexcept
{
    if (!QosTraceEnabled()) return;
    // Diagnostics must never change media behavior if formatting or enqueueing fails.
    try { GlobalLogger().WriteEvent(__FILE__, __LINE__, BaseLogger::INFO, event, "media quality diagnostic", fields); }
    catch (...) {}
}

bool QosTrace::SampleDue(uint64_t now_ms) const
{
    return !previous_ || now_ms < previous_->at_time_ms || now_ms - previous_->at_time_ms >= 1000;
}

void QosTrace::Observe(uint64_t id, const TransportDiagnostics& snapshot,
                       const transport::PacedSendQueue::Stats* queue)
{
    const auto& input = snapshot.controller;
    const ControlState state{snapshot.path_epoch, snapshot.network_available, input.feedback_valid, input.usage, input.feedback_source};
    if (!control_state_ || *control_state_ != state)
    {
        emit_("qos_control_state", ControlFields(id, snapshot));
        control_state_ = state;
    }
    if (snapshot.last_probe_result)
    {
        const auto& result = *snapshot.last_probe_result;
        const auto identity = std::make_pair(result.generation, result.cluster_id);
        if (!probe_result_ || *probe_result_ != identity)
        {
            emit_("qos_probe_result", {{"transport_id", id}, {"monotonic_ms", result.at_time_ms},
                {"path_epoch", snapshot.path_epoch}, {"cluster_id", result.cluster_id},
                {"generation", result.generation}, {"outcome", OutcomeName(result.outcome)},
                {"capacity_bps", result.outcome == ProbeOutcome::Succeeded ? std::optional<uint32_t>(result.capacity_bps) : std::nullopt}});
            probe_result_ = identity;
        }
    }
    if (!SampleDue(snapshot.at_time_ms)) return;
    auto fields = ControlFields(id, snapshot);
    std::optional<uint64_t> window;
    std::optional<double> sent_bps, sent_pps, probe_bps;
    if (previous_ && snapshot.path_epoch == previous_->path_epoch && snapshot.at_time_ms > previous_->at_time_ms &&
        snapshot.sent_rtp_bytes_total >= previous_->sent_rtp_bytes_total &&
        snapshot.sent_packets_total >= previous_->sent_packets_total && snapshot.sent_probe_bytes_total >= previous_->sent_probe_bytes_total)
    {
        window = snapshot.at_time_ms - previous_->at_time_ms;
        sent_bps = static_cast<double>(snapshot.sent_rtp_bytes_total - previous_->sent_rtp_bytes_total) * 8000 / *window;
        sent_pps = static_cast<double>(snapshot.sent_packets_total - previous_->sent_packets_total) * 1000 / *window;
        probe_bps = static_cast<double>(snapshot.sent_probe_bytes_total - previous_->sent_probe_bytes_total) * 8000 / *window;
    }
    const auto add = [&fields](const char* key, const auto& value) { fields.emplace_back(key, value); };
    add("window_ms", window); add("sent_rtp_bps", sent_bps); add("sent_packets_per_second", sent_pps);
    add("sent_probe_rtp_bps", probe_bps); add("sent_packets_total", snapshot.sent_packets_total);
    add("sent_rtp_bytes_total", snapshot.sent_rtp_bytes_total); add("twcc_reports_total", snapshot.twcc_reports_total);
    add("rr_reports_total", snapshot.rr_reports_total); add("matched_feedback_packets_total", snapshot.matched_feedback_packets_total);
    add("received_feedback_packets_total", snapshot.received_feedback_packets_total);
    add("unmatched_twcc_packets_total", snapshot.unmatched_twcc_packets_total);
    add("active_probe_cluster_id", snapshot.active_probe ? std::optional<int>(snapshot.active_probe->cluster_id) : std::nullopt);
    add("pacing_bitrate_bps", queue ? std::optional<uint32_t>(queue->pacing_bitrate_bps) : std::nullopt);
    add("queued_bytes", queue ? std::optional<uint64_t>(queue->queued_bytes) : std::nullopt);
    add("queued_packets", queue ? std::optional<uint64_t>(queue->queued_packets) : std::nullopt);
    add("oldest_wait_ms", queue ? std::optional<uint64_t>(queue->oldest_wait_ms) : std::nullopt);
    add("estimated_drain_ms", queue ? queue->estimated_drain_ms : std::nullopt);
    add("rejected_packets_total", queue ? std::optional<uint64_t>(queue->rejected_packets) : std::nullopt);
    emit_("qos_sample", fields);
    previous_ = snapshot;
}
}
