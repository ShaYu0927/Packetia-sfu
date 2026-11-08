#include "DownlinkBandwidthAllocator.h"

#include <algorithm>
#include <limits>
#include <stdexcept>
#include <unordered_set>

namespace media
{
DownlinkBandwidthAllocator::DownlinkBandwidthAllocator(Config config) : config_(config)
{
    if (!config.usable_percent || config.usable_percent > 100 ||
        config.resume_margin_percent > 100 || !config.feedback_timeout_ms)
        throw std::invalid_argument("invalid downlink allocation config");
}

std::vector<DownlinkAllocation> DownlinkBandwidthAllocator::Allocate(
    const DownlinkBandwidthBudget& budget, const std::vector<DownlinkDemand>& demands, uint64_t now_ms)
{
    const bool changed_path = epoch_ != budget.path_epoch;
    epoch_ = budget.path_epoch;
    const bool valid = budget.network_available && budget.estimate_valid &&
        budget.target_bps && now_ms >= budget.measured_at_ms &&
        now_ms - budget.measured_at_ms <= config_.feedback_timeout_ms;

    std::unordered_set<std::string> present;
    std::vector<const DownlinkDemand*> videos;
    uint64_t remaining = static_cast<uint64_t>(budget.target_bps) * config_.usable_percent / 100;
    for (const auto& demand : demands)
    {
        present.insert(demand.id);
        auto& slot = slots_[demand.id];
        if (changed_path || !valid || !demand.eligible) slot.pending = false;
        if (!demand.eligible) continue;
        if (demand.audio)
        {
            slot.allowed = true;
            remaining -= std::min<uint64_t>(remaining, demand.required_bps);
        }
        else videos.push_back(&demand);
    }
    for (auto it = slots_.begin(); it != slots_.end();)
        if (!present.count(it->first)) it = slots_.erase(it); else ++it;

    std::sort(videos.begin(), videos.end(), [&](const auto* a, const auto* b) {
        if (a->priority != b->priority) return a->priority > b->priority;
        // Stable allocations win ties; IDs make results independent of map order.
        if (slots_.at(a->id).allowed != slots_.at(b->id).allowed)
            return slots_.at(a->id).allowed;
        return a->id < b->id;
    });
    if (valid)
    {
        std::vector<const DownlinkDemand*> resume_candidates;
        for (const auto* demand : videos)
        {
            auto& slot = slots_.at(demand->id);
            const uint64_t cost = static_cast<uint64_t>(demand->required_bps) *
                (slot.allowed ? 100 : 100 + config_.resume_margin_percent) / 100;
            // Unknown demand cannot be interpreted as a zero-cost video.
            const bool wanted = cost != 0 && cost <= remaining;
            if (wanted) remaining -= cost;
            if (wanted == slot.allowed) { slot.pending = false; continue; }
            if (!slot.pending || now_ms < slot.since_ms)
            {
                slot.pending = true;
                slot.since_ms = now_ms;
            }
            const auto delay = wanted ? config_.resume_delay_ms : config_.pause_delay_ms;
            if (now_ms - slot.since_ms >= delay)
            {
                if (wanted) resume_candidates.push_back(demand);
                else { slot.allowed = false; slot.pending = false; }
            }
        }
        // A track waiting for its pause delay is still sending. Account for it
        // before activating another track, even when the desired allocation
        // above has already assigned its capacity to a higher-priority track.
        uint64_t available = static_cast<uint64_t>(budget.target_bps) * config_.usable_percent / 100;
        for (const auto& demand : demands)
            if (demand.eligible && (demand.audio || slots_.at(demand.id).allowed))
                available -= std::min<uint64_t>(available, demand.required_bps);
        for (const auto* demand : resume_candidates)
        {
            const uint64_t cost = static_cast<uint64_t>(demand->required_bps) *
                (100 + config_.resume_margin_percent) / 100;
            if (cost <= available)
            {
                available -= cost;
                auto& slot = slots_.at(demand->id);
                slot.allowed = true;
                slot.pending = false;
            }
        }
    }
    std::vector<DownlinkAllocation> result;
    for (const auto& demand : demands) result.push_back({demand.id, slots_.at(demand.id).allowed});
    return result;
}

SourceBitrateEstimator::SourceBitrateEstimator()
{
    // About 25% new weight at the usual 500 ms sampling interval.
    smoothing_.SetTimeConstantMs(1750);
}

void SourceBitrateEstimator::Observe(size_t bytes, uint64_t now_ms)
{
    if (!started_ || now_ms < start_ms_ || now_ms - start_ms_ > 3000)
    {
        started_ = true;
        start_ms_ = now_ms;
        bytes_ = 0;
        smoothing_.Reset();
    }
    bytes_ += std::min<uint64_t>(bytes, UINT32_MAX);
    const auto elapsed = now_ms - start_ms_;
    if (elapsed < 500) return;
    const uint32_t sample = static_cast<uint32_t>(std::min<uint64_t>(bytes_ * 8000 / elapsed, UINT32_MAX));
    smoothing_.AddSample(sample, now_ms);
    bitrate_.store(static_cast<uint32_t>(*smoothing_.GetValue(now_ms)), std::memory_order_relaxed);
    start_ms_ = now_ms;
    bytes_ = 0;
}
}
