#pragma once

#include "NetworkStats.h"
#include "ProbeController.h"

namespace media
{
// Read-only observations. Missing or expired measurements remain unknown.
struct ControllerDiagnostics
{
    NetworkControlUpdate control;
    bool feedback_valid = false;
    bool bwe_estimate_valid = false;
    const char* feedback_source = "none";
    std::optional<uint64_t> feedback_time_ms;
    std::optional<uint64_t> twcc_time_ms;
    std::optional<double> loss_rate, rtt_ms, jitter_ms, delay_trend_ms, delay_slope;
    std::optional<uint32_t> receive_bitrate_bps;
    std::optional<BandwidthUsage> usage;
};

struct TransportDiagnostics
{
    uint64_t at_time_ms = 0;
    uint64_t path_epoch = 0;
    bool network_available = false;
    // Lifetime totals include only accepted send completions on the current path.
    // Totals survive route changes; interval rates must restart at a path boundary.
    uint64_t sent_packets_total = 0, sent_rtp_bytes_total = 0, sent_probe_bytes_total = 0;
    uint64_t twcc_reports_total = 0, rr_reports_total = 0;
    uint64_t matched_feedback_packets_total = 0, received_feedback_packets_total = 0;
    uint64_t unmatched_twcc_packets_total = 0;
    ControllerDiagnostics controller;
    std::optional<ProbePlan> active_probe;
    std::optional<ProbeResult> last_probe_result;
};
}
