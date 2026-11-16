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
    const bool path_changed = epoch_ != budget.path_epoch;
    epoch_ = budget.path_epoch;
    const bool budget_valid = IsBudgetValid(budget, now_ms);
    const bool reset_pending = path_changed || !budget_valid;

    auto plans = Prepare(demands, reset_pending);

    const uint64_t usable_bps = static_cast<uint64_t>(budget.target_bps) * config_.usable_percent / 100;
    std::vector<AllocationPlan*> videos;
    if (budget_valid)
    {
        videos = ProvisionalAllocate(plans, usable_bps);
    }

    return Commit(plans, videos, usable_bps, budget_valid, now_ms);
}

bool DownlinkBandwidthAllocator::IsBudgetValid(const DownlinkBandwidthBudget& budget, uint64_t now_ms) const
{
    if (!budget.network_available)
        return false;

    if (!budget.estimate_valid || budget.target_bps == 0)
        return false;

    // 先检查时间顺序，避免无符号减法下溢。
    if (now_ms < budget.measured_at_ms)
        return false;

    const uint64_t budget_age_ms = now_ms - budget.measured_at_ms;
    return budget_age_ms <= config_.feedback_timeout_ms;
}

std::vector<DownlinkBandwidthAllocator::AllocationPlan> DownlinkBandwidthAllocator::Prepare(
    const std::vector<DownlinkDemand>& demands, bool reset_pending)
{
    std::unordered_set<std::string> present;
    present.reserve(demands.size());
    std::vector<AllocationPlan> plans;
    plans.reserve(demands.size());
    for (const auto& demand : demands)
    {
        present.insert(demand.id);
        auto& slot = slots_[demand.id];
        if (reset_pending || !demand.eligible) slot.pending = false;
        plans.push_back({&demand, &slot, slot.allowed});
    }
    for (auto it = slots_.begin(); it != slots_.end();)
        if (!present.count(it->first)) it = slots_.erase(it); else ++it;
    return plans;
}

std::vector<DownlinkBandwidthAllocator::AllocationPlan*> DownlinkBandwidthAllocator::ProvisionalAllocate(
    std::vector<AllocationPlan>& plans, uint64_t usable_bps) const
{
    uint64_t remaining = usable_bps;
    std::vector<AllocationPlan*> videos;
    videos.reserve(plans.size());
    for (auto& plan : plans)
    {
        const auto& demand = *plan.demand;
        if (!demand.eligible) continue;
        if (demand.audio)
        {
            // Audio and policy-exempt tracks reserve their cost before managed video.
            plan.wanted = true;
            plan.reserved_bps = demand.required_bps;
            remaining -= std::min(remaining, plan.reserved_bps);
        }
        else videos.push_back(&plan);
    }
    std::sort(videos.begin(), videos.end(), [](const auto* a, const auto* b) {
        if (a->demand->priority != b->demand->priority) return a->demand->priority > b->demand->priority;
        if (a->slot->allowed != b->slot->allowed) return a->slot->allowed;
        return a->demand->id < b->demand->id;
    });
    for (auto* plan : videos)
    {
        // One candidate per video today. A future layer allocator can try lower
        // layers for all tracks before upgrades, using the same prepare/commit boundary.
        const uint64_t cost = static_cast<uint64_t>(plan->demand->required_bps) *
            (plan->slot->allowed ? 100 : 100 + config_.resume_margin_percent) / 100;
        // An unknown source rate is not a free video.
        plan->wanted = cost != 0 && cost <= remaining;
        if (plan->wanted)
        {
            plan->reserved_bps = cost;
            remaining -= cost;
        }
    }
    return videos;
}

std::vector<DownlinkAllocation> DownlinkBandwidthAllocator::Commit(
    std::vector<AllocationPlan>& plans, const std::vector<AllocationPlan*>& videos,
    uint64_t usable_bps, bool valid, uint64_t now_ms)
{
    for (auto& plan : plans)
    {
        const auto& demand = *plan.demand;
        auto& slot = *plan.slot;
        if (!demand.eligible) continue;
        if (demand.audio)
        {
            slot.allowed = true;
            slot.pending = false;
            continue;
        }
        if (!valid) continue; // Missing feedback holds committed forwarding state.
        if (plan.wanted == slot.allowed) { slot.pending = false; continue; }
        if (!slot.pending || now_ms < slot.since_ms)
        {
            slot.pending = true;
            slot.since_ms = now_ms;
        }
        const auto delay = plan.wanted ? config_.resume_delay_ms : config_.pause_delay_ms;
        if (now_ms - slot.since_ms >= delay)
        {
            if (plan.wanted) plan.resume_ready = true;
            else { slot.allowed = false; slot.pending = false; }
        }
    }

    // A provisional plan may already reuse bandwidth from a planned pause.
    // Commit must first account for tracks still sending during their pause delay.
    uint64_t available = usable_bps;
    for (const auto& plan : plans)
        if (plan.demand->eligible && plan.slot->allowed)
            available -= std::min<uint64_t>(available, plan.demand->required_bps);
    for (auto* plan : videos)
    {
        if (plan->resume_ready && plan->reserved_bps <= available)
        {
            available -= plan->reserved_bps;
            plan->slot->allowed = true;
            plan->slot->pending = false;
        }
    }

    std::vector<DownlinkAllocation> result;
    result.reserve(plans.size());
    for (auto& plan : plans)
    {
        auto& slot = *plan.slot;
        const uint32_t requested = plan.demand->eligible && slot.allowed ? plan.demand->required_bps : 0;
        const int64_t delta = static_cast<int64_t>(requested) - slot.bandwidth_requested_bps;
        slot.bandwidth_requested_bps = requested;
        result.push_back({plan.demand->id, slot.allowed, requested, delta});
    }
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
