#include "WeakNetController.h"
#include <algorithm>

namespace media
{
void WeakNetController::SetBitrateConstraints(const BitrateConstraints& constraints)
{
    bitrate_ = constraints;
    target_bitrate_bps_ = std::clamp(target_bitrate_bps_,
                                     bitrate_.min_bitrate_bps,
                                     bitrate_.max_bitrate_bps);
}

NetworkControlUpdate WeakNetController::OnFeedback(const WeakNetFeedback& feedback)
{
    NetworkControlUpdate update;

    const auto quality = EstimateQuality(feedback);
    const uint32_t target = UpdateTargetBitrate(feedback, quality);

    snapshot_.update_time_ms = feedback.now_ms;
    snapshot_.send_bitrate_bps = feedback.send_bitrate_bps;
    snapshot_.recv_bitrate_bps = feedback.receive_bitrate_bps;
    snapshot_.target_bitrate_bps = target;
    snapshot_.rtt_ms = feedback.rtt_ms;
    snapshot_.jitter_ms = feedback.jitter_ms;
    snapshot_.loss_rate = feedback.loss_rate;
    snapshot_.nack_count = feedback.nack_count;
    snapshot_.pli_count = feedback.pli_count;
    snapshot_.fir_count = feedback.fir_count;
    snapshot_.quality = quality;

    update.has_target_rate = true;
    update.target_rate.update_time_ms = feedback.now_ms;
    update.target_rate.target_bitrate_bps = target;
    update.target_rate.stable_target_bitrate_bps = stable_target_bitrate_bps_;
    update.target_rate.estimate.update_time_ms = feedback.now_ms;
    update.target_rate.estimate.send_bitrate_bps = feedback.send_bitrate_bps;
    update.target_rate.estimate.recv_bitrate_bps = feedback.receive_bitrate_bps;
    update.target_rate.estimate.rtt_ms = feedback.rtt_ms;
    update.target_rate.estimate.jitter_ms = feedback.jitter_ms;
    update.target_rate.estimate.loss_rate = feedback.loss_rate;
    update.target_rate.estimate.quality = quality;

    update.has_pacer_config = true;
    update.pacer_config.update_time_ms = feedback.now_ms;
    update.pacer_config.pacing_bitrate_bps = target * 12 / 10;
    update.pacer_config.time_window_ms = 40;

    update.enable_nack = true;
    update.enable_fec = quality == NetworkQualityLevel::Bad;
    update.request_key_frame = ShouldRequestKeyFrame(feedback, quality);

    return update;
}

NetworkQualityLevel WeakNetController::EstimateQuality(const WeakNetFeedback& feedback) const
{
    if (feedback.loss_rate >= 0.20 || feedback.rtt_ms >= 500)
    {
        return NetworkQualityLevel::Bad;
    }

    if (feedback.loss_rate >= 0.08 || feedback.rtt_ms >= 300 || feedback.jitter_ms >= 80)
    {
        return NetworkQualityLevel::Weak;
    }

    if (feedback.loss_rate >= 0.02 || feedback.rtt_ms >= 150 || feedback.jitter_ms >= 40)
    {
        return NetworkQualityLevel::Good;
    }

    return NetworkQualityLevel::Excellent;
}

uint32_t WeakNetController::UpdateTargetBitrate(const WeakNetFeedback& feedback, NetworkQualityLevel quality)
{
    uint32_t base = target_bitrate_bps_;

    switch (quality)
    {
    case NetworkQualityLevel::Excellent:
        // Ar(ti) = eta * Ar(ti-1), eta = 1.05
        base = static_cast<uint32_t>(target_bitrate_bps_ * 1.05);
        break;

    case NetworkQualityLevel::Good:
        // Ar(ti) = Ar(ti-1)
        break;

    case NetworkQualityLevel::Weak:
        // Ar(ti) = alpha * Rr(ti), alpha = 0.85
        base = static_cast<uint32_t>(feedback.receive_bitrate_bps * 0.85);
        break;

    case NetworkQualityLevel::Bad:
        base = static_cast<uint32_t>(feedback.receive_bitrate_bps * 0.85);
        break;

    default:
        break;
    }

    target_bitrate_bps_ = std::clamp(base, bitrate_.min_bitrate_bps, bitrate_.max_bitrate_bps);
    stable_target_bitrate_bps_ = static_cast<uint32_t>(
        static_cast<uint64_t>(stable_target_bitrate_bps_) * 8 / 10 +
        static_cast<uint64_t>(target_bitrate_bps_) * 2 / 10);

    return target_bitrate_bps_;
}

bool WeakNetController::ShouldRequestKeyFrame(const WeakNetFeedback& feedback, NetworkQualityLevel quality)
{
    if (quality != NetworkQualityLevel::Bad)
    {
        return false;
    }

    if (last_keyframe_request_ms_ != 0 &&
        feedback.now_ms >= last_keyframe_request_ms_ &&
        feedback.now_ms - last_keyframe_request_ms_ < keyframe_interval_ms_)
    {
        return false;
    }

    last_keyframe_request_ms_ = feedback.now_ms;
    return true;
}

}
