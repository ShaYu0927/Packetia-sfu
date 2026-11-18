#pragma once

#include "bwe/PacketGroupObserver.h"

#include <map>
#include <optional>

namespace media
{
struct ProbeRequest
{
    uint64_t now_ms = 0;
    uint64_t path_epoch = 0;
    uint64_t active_bitrate_bps = 0;
    uint32_t candidate_bitrate_bps = 0;
    // Connection budget needed to preserve active demand and resume the candidate.
    // Missing values retain the legacy demand-based plan for independent callers.
    std::optional<uint64_t> required_budget_bps;
};

struct ProbeContext
{
    ProbeRequest request;
    BitrateConstraints constraints;
    uint32_t target_bitrate_bps = 0;
    uint32_t rtt_ms = 0;
    std::optional<BweResult> bandwidth_estimate;
    // Local receive time of the TWCC that produced bandwidth_estimate.
    std::optional<uint64_t> bandwidth_estimate_time_ms;
};

struct ProbePlan
{
    // Extra padding budget, excluding active media.
    uint32_t bitrate_bps = 0;
    uint32_t duration_ms = 0;
    uint64_t max_bytes = 0;
    // Framework-assigned identity, never reused during a transport lifetime.
    int cluster_id = -1;
    uint64_t generation = 0;
    // Total pacing rate while executing, including active media.
    uint32_t pacing_bitrate_bps = 0;
};

enum class ProbeDecisionReason
{
    Unspecified,
    Planned,
    InProgress,
    CoolingDown,
    Congested,
    NoDemand,
    NoHeadroom,
    RecoveryExceedsMaximum,
    TooSmall,
    NetworkUnavailable,
    StalePath,
};

const char* ToString(ProbeDecisionReason reason);

struct ProbeDecision
{
    // Unhandled decisions use the legacy policy; handled + no plan suppresses probing.
    bool handled = false;
    std::optional<ProbePlan> plan;
    ProbeDecisionReason reason = ProbeDecisionReason::Unspecified;
};

enum class ProbeEventType { Started, Finished, Aborted };

struct ProbeEvent
{
    ProbeEventType type = ProbeEventType::Started;
    ProbePlan plan;
    uint64_t path_epoch = 0;
    uint64_t at_time_ms = 0;
    // Execution accounting includes conservative header/crypto overhead.
    uint64_t accounted_bytes = 0;
};

// Execution completion alone is never evidence of available capacity.
enum class ProbeOutcome { Succeeded, Inconclusive, Congested, TimedOut, Aborted };

struct ProbeResult
{
    int cluster_id = -1;
    uint64_t generation = 0;
    ProbeOutcome outcome = ProbeOutcome::Inconclusive;
    uint32_t capacity_bps = 0;
    uint64_t at_time_ms = 0;
};

// Transport-owned; called under the SendSideController mutex, without I/O or callbacks.
class IProbeController
{
public:
    virtual ~IProbeController() = default;
    virtual ProbeDecision OnProcessInterval(const ProbeContext& context) = 0;
    virtual void OnFeedback(const TransportFeedback& feedback,
                            const PacketGroupObservation& groups, const BweResult& estimate) = 0;
    virtual void OnProbeEvent(const ProbeEvent& event) = 0;
    virtual void Reset() = 0;
    virtual void OnTime(uint64_t) {}
    virtual std::optional<ProbeResult> TakeResult() { return std::nullopt; }
};

class ProbeController final : public IProbeController
{
public:
    ProbeDecision OnProcessInterval(const ProbeContext& context) override;
    void OnFeedback(const TransportFeedback& feedback,
                    const PacketGroupObservation& groups, const BweResult& estimate) override;
    void OnProbeEvent(const ProbeEvent& event) override;
    void Reset() override;
    void OnTime(uint64_t now_ms) override;
    std::optional<ProbeResult> TakeResult() override;

private:
    struct Cluster
    {
        ProbePlan plan;
        uint64_t started_ms = 0;
        std::optional<uint64_t> finished_ms;
        uint64_t accounted_bytes = 0;
        uint32_t feedback_wait_ms = 1000;
        std::map<int64_t, PacketFeedback> packets;
    };
    ProbeDecisionReason CheckStartConditions(const ProbeContext& context) const;
    ProbeDecision BuildPlan(const ProbeContext& context) const;
    void Evaluate(uint64_t now_ms);
    void Complete(ProbeOutcome outcome, uint64_t now_ms, uint32_t capacity_bps = 0);
    std::optional<Cluster> cluster_;
    std::optional<ProbeResult> result_;
    std::optional<uint64_t> last_completed_ms_;
    uint32_t cooldown_ms_ = 3000;
    uint32_t feedback_wait_ms_ = 1000;
};
}
