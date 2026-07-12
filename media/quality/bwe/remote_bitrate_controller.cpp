#include "remote_bitrate_controller.h"
#include <algorithm>

BweResult RemoteBitrateController::Update(BandwidthUsage usage,
                                           uint32_t incoming_bitrate_bps,
                                           double loss_rate,
                                           int64_t now_ms)
{
    uint32_t next_bitrate_bps = target_bitrate_bps_;

    switch (usage)
    {
    case BandwidthUsage::kOverUsing:
        state_ = RateControlState::kDecrease;
        if (incoming_bitrate_bps > 0)
        {
            next_bitrate_bps = std::min(next_bitrate_bps, incoming_bitrate_bps);
        }
        next_bitrate_bps = static_cast<uint32_t>(next_bitrate_bps * 0.85);
        break;

    case BandwidthUsage::kUnderUsing:
        state_ = RateControlState::kHold;
        break;

    case BandwidthUsage::kNormal:
    default:
        state_ = RateControlState::kIncrease;
        next_bitrate_bps = static_cast<uint32_t>(next_bitrate_bps * 1.05);
        break;
    }

    next_bitrate_bps = loss_controller_.Update(next_bitrate_bps, loss_rate);
    target_bitrate_bps_ = std::max(min_bitrate_bps_, std::min(next_bitrate_bps, max_bitrate_bps_));
    last_update_ms_ = now_ms;

    BweResult result;
    result.target_bitrate_bps = target_bitrate_bps_;
    result.usage = usage;
    result.rc_state = state_;
    result.loss_rate = loss_rate;
    return result;
}