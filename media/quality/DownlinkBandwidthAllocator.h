#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>
#include "estimator/EwmaFilter.h"

namespace media
{

struct DownlinkBandwidthBudget
{
    uint64_t path_epoch = 1;          // Network path generation, used to identify path changes.
    uint64_t measured_at_ms = 0;      // Monotonic timestamp of the bandwidth estimate, in milliseconds.
    bool network_available = true;    // Whether the network is currently available.
    bool estimate_valid = false;      // Whether a valid bandwidth estimate has been obtained.
    uint32_t target_bps = 0;          // Current target sending bitrate for this receiver, in bits per second.
    uint32_t start_bps = 500000;      // Initial sending bitrate before an estimate is available, in bits per second.
    uint32_t max_bps = 2000000;       // Maximum permitted sending bitrate for this receiver, in bits per second.
    uint64_t probe_generation = 1;    // Invalidates probe execution when feedback state resets.
};

struct DownlinkSubscriptionPolicy
{
    bool enabled = true;            // Whether this subscription is enabled.
    uint8_t priority = 128;          // Allocation priority; higher values take precedence.
    uint32_t bitrate_hint_bps = 0;   // Estimated bitrate demand in bits per second; 0 means unspecified.
};

struct DownlinkDemand
{
    std::string id;               // Unique identifier for the subscription.
    bool audio = false;           // Whether this demand represents an audio stream.
    bool eligible = true;         // Whether this subscription is eligible for allocation.
    uint8_t priority = 128;       // Allocation priority; higher values take precedence.
    uint32_t required_bps = 0;    // Required bitrate in bits per second; 0 means unknown.
};

struct DownlinkAllocation
{
    std::string id;
    bool allowed = true;
    // Committed demand, excluding the extra headroom required to resume.
    uint32_t bandwidth_requested_bps = 0;
    int64_t bandwidth_delta_bps = 0; // Change from this track's previous committed demand.
};


class DownlinkBandwidthAllocator
{
public:
    struct Config
    {
        uint32_t usable_percent = 85; 
        uint32_t resume_margin_percent = 20;
        uint64_t pause_delay_ms = 300;
        uint64_t resume_delay_ms = 1000;
        uint64_t feedback_timeout_ms = 1500;
    };
    DownlinkBandwidthAllocator() = default;
    explicit DownlinkBandwidthAllocator(Config config);
    std::vector<DownlinkAllocation> Allocate(const DownlinkBandwidthBudget& budget, const std::vector<DownlinkDemand>& demands, uint64_t now_ms);
    void Remove(const std::string& id) { slots_.erase(id); }
    void Clear() { slots_.clear(); epoch_ = 0; }

private:
    struct Slot
    {
        bool allowed = true;
        bool pending = false;
        uint64_t since_ms = 0;
        uint32_t bandwidth_requested_bps = 0;
    };

    // Single-layer counterpart of LiveKit's provisional allocation state.
    // Planning does not change Slot::allowed; only Commit changes forwarding permission.
    struct AllocationPlan
    {
        const DownlinkDemand* demand;
        Slot* slot;
        bool wanted;
        uint64_t reserved_bps = 0;
        bool resume_ready = false;
    };

    bool IsBudgetValid(const DownlinkBandwidthBudget& budget, uint64_t now_ms) const;
    std::vector<AllocationPlan> Prepare(const std::vector<DownlinkDemand>& demands, bool reset_pending);
    std::vector<AllocationPlan*> ProvisionalAllocate(std::vector<AllocationPlan>& plans, uint64_t usable_bps) const;
    std::vector<DownlinkAllocation> Commit(std::vector<AllocationPlan>& plans,
        const std::vector<AllocationPlan*>& videos, uint64_t usable_bps, bool valid, uint64_t now_ms);

    Config config_;
    uint64_t epoch_ = 0;
    std::unordered_map<std::string, Slot> slots_;
};

class SourceBitrateEstimator
{
public:
    SourceBitrateEstimator();
    void Observe(size_t bytes, uint64_t now_ms);
    uint32_t Bitrate() const { return bitrate_.load(std::memory_order_relaxed); }
private:
    std::atomic<uint32_t> bitrate_{0};
    EwmaFilter smoothing_{0.75};
    bool started_ = false;
    uint64_t start_ms_ = 0;
    uint64_t bytes_ = 0;
};
}
