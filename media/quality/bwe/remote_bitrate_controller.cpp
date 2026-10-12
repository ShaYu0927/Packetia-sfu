#include "remote_bitrate_controller.h"
#include <algorithm>
#include <cmath>
#include <stdexcept>

double RemoteBitrateController::ClampRate(double rate) const
{
    if (!std::isfinite(rate)) rate = min_bitrate_bps_;
    return std::clamp(rate, static_cast<double>(min_bitrate_bps_), static_cast<double>(max_bitrate_bps_));
}

void RemoteBitrateController::SetBitrateConstraints(uint32_t min_bps, uint32_t start_bps, uint32_t max_bps)
{
    if (!min_bps || min_bps > start_bps || start_bps > max_bps)
        throw std::invalid_argument("invalid bandwidth bitrate constraints");
    min_bitrate_bps_ = min_bps;
    start_bitrate_bps_ = start_bps;
    max_bitrate_bps_ = max_bps;
    Reset();
}

void RemoteBitrateController::Reset()
{
    state_ = RateControlState::kIncrease;
    region_ = RateControlRegion::kUnknown;
    target_bitrate_bps_ = start_bitrate_bps_;
    average_rtt_ms_ = 0;
    capacity_kbps_ = -1;
    capacity_variance_ = 0.4;
    max_hold_rate_ = 0;
    last_update_ms_ = first_incoming_ms_ = loss_probe_start_ms_ = -1;
    initial_incoming_bps_ = 0;
    initialized_ = false;
    baseline_loss_ = loss_before_probe_ = rate_before_probe_ = 0;
}

void RemoteBitrateController::SetRtt(uint32_t rtt_ms)
{
    if (!rtt_ms) return;
    average_rtt_ms_ = average_rtt_ms_ == 0 ? rtt_ms : (average_rtt_ms_ + rtt_ms) * 0.5;
}

void RemoteBitrateController::UpdateCapacity(double incoming_bps)
{
    if (incoming_bps <= 0) return;
    const double kbps = incoming_bps / 1000.0;
    if (capacity_kbps_ < 0) capacity_kbps_ = kbps;
    else capacity_kbps_ = 0.95 * capacity_kbps_ + 0.05 * kbps;
    const double difference = capacity_kbps_ - kbps;
    capacity_variance_ = std::clamp(0.95 * capacity_variance_ +
        0.05 * difference * difference / std::max(capacity_kbps_, 1.0), 0.4, 2.5);
}

BweResult RemoteBitrateController::Update(BandwidthUsage usage, uint32_t incoming_bitrate_bps,
                                         double loss_rate, int64_t now_ms, uint32_t rtt_ms)
{
    loss_rate = std::isfinite(loss_rate) ? std::clamp(loss_rate, 0.0, 1.0) : 1.0;
    auto result = [&] {
        BweResult out;
        out.target_bitrate_bps = target_bitrate_bps();
        out.usage = usage;
        out.rc_state = state_;
        out.loss_rate = loss_rate;
        out.incoming_bitrate_bps = incoming_bitrate_bps;
        out.estimate_valid = initialized_;
        out.region = region_;
        return out;
    };
    // Duplicated/backwards feedback cannot advance the control clock.
    if (now_ms < 0 || (last_update_ms_ >= 0 && now_ms <= last_update_ms_)) return result();
    SetRtt(rtt_ms);
    double elapsed_ms = last_update_ms_ < 0 ? 0 : std::min<int64_t>(now_ms - last_update_ms_, 3000);
    last_update_ms_ = now_ms;
    if (incoming_bitrate_bps) {
        if (first_incoming_ms_ < 0) first_incoming_ms_ = now_ms;
        initial_incoming_bps_ = std::max(initial_incoming_bps_, incoming_bitrate_bps);
        if (!initialized_ && (usage == BandwidthUsage::kOverUsing || now_ms - first_incoming_ms_ >= 2000)) {
            const double initial_rate = usage == BandwidthUsage::kOverUsing ?
                incoming_bitrate_bps : initial_incoming_bps_;
            // Finishing warmup must not undo a reduction during loss/queueing.
            target_bitrate_bps_ = ClampRate(usage != BandwidthUsage::kNormal ||
                loss_probe_start_ms_ >= 0 || loss_rate > 0.10 ?
                std::min(target_bitrate_bps_, initial_rate) : initial_rate);
            initialized_ = true;
            elapsed_ms = 0; // Startup observation time was not time spent probing upward.
        }
    }
    const double incoming = incoming_bitrate_bps;
    const double compensated = incoming / (1.0 - std::min(0.95 * baseline_loss_, 0.6));
    const double beta = region_ == RateControlRegion::kNearMax ? 0.8 : 0.85;
    const auto previous_state = state_;
    if (usage == BandwidthUsage::kOverUsing) {
        state_ = RateControlState::kDecrease;
        // New delay congestion invalidates the pre-loss recovery anchor.
        loss_probe_start_ms_ = -1;
    } else if (usage == BandwidthUsage::kUnderUsing || previous_state == RateControlState::kDecrease ||
               loss_probe_start_ms_ >= 0)
        state_ = RateControlState::kHold;
    else state_ = RateControlState::kIncrease;

    if (state_ == RateControlState::kDecrease) {
        const double anchor = incoming > 0 ? std::min(target_bitrate_bps_, compensated) : target_bitrate_bps_;
        // Clamp after signed arithmetic: the reference's unsigned -2000 can wrap.
        target_bitrate_bps_ = previous_state != RateControlState::kDecrease ?
            beta * anchor - 2000.0 : anchor * std::pow(beta, elapsed_ms / 1000.0);
        if (capacity_kbps_ > 0 && compensated / 1000.0 < 0.5 * capacity_kbps_) capacity_kbps_ = -1;
        UpdateCapacity(compensated);
        region_ = RateControlRegion::kNearMax;
        max_hold_rate_ = 0;
    } else if (state_ == RateControlState::kHold) {
        max_hold_rate_ = std::max(max_hold_rate_, target_bitrate_bps_);
        UpdateCapacity(compensated);
    } else if (initialized_ && incoming > 0 && loss_probe_start_ms_ < 0) {
        if (capacity_kbps_ > 0 && compensated / 1000.0 > capacity_kbps_ +
            3.0 * std::sqrt(capacity_variance_ * capacity_kbps_)) region_ = RateControlRegion::kAboveMax;
        const double sensitivity = region_ == RateControlRegion::kNearMax ? 0.2 :
            (region_ == RateControlRegion::kAboveMax ? 0.5 : 1.0);
        const double reaction_ms = std::clamp(average_rtt_ms_ + 300.0, 300.0, 1000.0);
        const double growth = std::log(1.06) * sensitivity;
        // Continuous alpha*rate+delta: frequency changes do not multiply growth.
        const double alpha = std::exp(growth * elapsed_ms / reaction_ms);
        const double candidate = target_bitrate_bps_ * alpha +
            4500.0 * sensitivity / growth * (alpha - 1.0);
        const double radical_incoming = incoming / (1.0 - std::min(baseline_loss_, 0.8));
        if (candidate <= 1.7 * radical_incoming || target_bitrate_bps_ <= start_bitrate_bps_ * 0.5)
            target_bitrate_bps_ = candidate;
        if (max_hold_rate_ > 0) {
            target_bitrate_bps_ = std::max(target_bitrate_bps_, beta * max_hold_rate_);
            max_hold_rate_ = 0;
        }
    }
    if (loss_probe_start_ms_ >= 0) {
        const double observation_ms = std::clamp(average_rtt_ms_, 1000.0, 2000.0) + 500.0;
        if (now_ms - loss_probe_start_ms_ >= observation_ms) {
            baseline_loss_ = 0.85 * std::min(loss_rate, 0.8);
            if (usage == BandwidthUsage::kNormal && incoming > 0) {
                if (loss_rate < loss_before_probe_ && loss_rate < 0.3) {
                    target_bitrate_bps_ = std::min(rate_before_probe_,
                        target_bitrate_bps_ / std::max(1.0 - loss_rate, 0.2));
                    region_ = RateControlRegion::kNearMax;
                } else if (loss_rate < 0.3) target_bitrate_bps_ = rate_before_probe_;
            }
            loss_probe_start_ms_ = -1;
        }
    } else if (loss_rate > 0.10 && loss_rate > baseline_loss_ + 0.03 && usage != BandwidthUsage::kOverUsing) {
        rate_before_probe_ = target_bitrate_bps_;
        loss_before_probe_ = loss_rate;
        loss_probe_start_ms_ = now_ms;
        target_bitrate_bps_ = 0.85 * (incoming > 0 ? std::min(target_bitrate_bps_, incoming) : target_bitrate_bps_);
        state_ = RateControlState::kDecrease;
    }
    target_bitrate_bps_ = ClampRate(target_bitrate_bps_);
    return result();
}
