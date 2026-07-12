#include "NetworkController.h"
#include <algorithm>

namespace media
{

NetworkController::NetworkController()
{
    fallback_target_bitrate_bps_ = bitrate_.start_bitrate_bps;
    stable_target_bitrate_bps_ = bitrate_.start_bitrate_bps;
}

void NetworkController::SetBitrateConstraints(const BitrateConstraints& constraints)
{
    bitrate_ = constraints;
    fallback_target_bitrate_bps_ = std::max(bitrate_.min_bitrate_bps,
                                            std::min(fallback_target_bitrate_bps_, bitrate_.max_bitrate_bps));
    stable_target_bitrate_bps_ = std::max(bitrate_.min_bitrate_bps,
                                          std::min(stable_target_bitrate_bps_, bitrate_.max_bitrate_bps));
}

NetworkControlUpdate NetworkController::OnFeedback(const WeakNetFeedback& feedback)
{
    latest_feedback_ = feedback;
    has_feedback_ = true;

    NetworkControlUpdate update = BuildUpdate(nullptr, feedback);
    NotifyNetworkControlUpdate(update);
    return update;
}

NetworkControlUpdate NetworkController::OnBweResultAndFeedback(const BweResult& bwe, const WeakNetFeedback& feedback)
{
    latest_feedback_ = feedback;
    has_feedback_ = true;

    NetworkControlUpdate update = BuildUpdate(&bwe, feedback);
    NotifyNetworkControlUpdate(update);
    return update;
}

void NetworkController::OnBweResult(const BweResult& result)
{
    if (!has_feedback_)
    {
        return;
    }

    NetworkControlUpdate update = BuildUpdate(&result, latest_feedback_);
    NotifyNetworkControlUpdate(update);
}

void NetworkController::AddObserver(const std::shared_ptr<INetworkControlObserver>& observer)
{
    if (!observer)
    {
        return;
    }

    observers_.push_back(observer);
}

void NetworkController::RemoveObserver(const std::shared_ptr<INetworkControlObserver>& observer)
{
    observers_.erase(
        std::remove_if(observers_.begin(), observers_.end(),
                       [&](const std::weak_ptr<INetworkControlObserver>& weak_observer) {
                           auto locked = weak_observer.lock();
                           return !locked || locked == observer;
                       }),
        observers_.end());
}

NetworkQualityLevel NetworkController::EstimateQuality(const WeakNetFeedback& feedback) const
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

NetworkControlUpdate NetworkController::BuildUpdate(const BweResult* bwe, const WeakNetFeedback& feedback)
{
    const NetworkQualityLevel quality = EstimateQuality(feedback);

    uint32_t target_bitrate_bps = fallback_target_bitrate_bps_;
    if (bwe && bwe->target_bitrate_bps > 0)
    {
        target_bitrate_bps = bwe->target_bitrate_bps;
    }
    else
    {
        switch (quality)
        {
        case NetworkQualityLevel::Excellent:
            target_bitrate_bps = static_cast<uint32_t>(target_bitrate_bps * 1.05);
            break;
        case NetworkQualityLevel::Weak:
            target_bitrate_bps = static_cast<uint32_t>(target_bitrate_bps * 0.85);
            break;
        case NetworkQualityLevel::Bad:
            target_bitrate_bps = static_cast<uint32_t>(target_bitrate_bps * 0.65);
            break;
        case NetworkQualityLevel::Good:
        default:
            break;
        }
    }

    target_bitrate_bps = std::max(bitrate_.min_bitrate_bps,
                                  std::min(target_bitrate_bps, bitrate_.max_bitrate_bps));
    fallback_target_bitrate_bps_ = target_bitrate_bps;
    stable_target_bitrate_bps_ = static_cast<uint32_t>(stable_target_bitrate_bps_ * 0.8 +
                                                       target_bitrate_bps * 0.2);

    snapshot_.update_time_ms = feedback.now_ms;
    snapshot_.send_bitrate_bps = feedback.send_bitrate_bps;
    snapshot_.target_bitrate_bps = target_bitrate_bps;
    snapshot_.rtt_ms = feedback.rtt_ms;
    snapshot_.jitter_ms = feedback.jitter_ms;
    snapshot_.loss_rate = feedback.loss_rate;
    snapshot_.nack_count = feedback.nack_count;
    snapshot_.pli_count = feedback.pli_count;
    snapshot_.fir_count = feedback.fir_count;
    snapshot_.quality = quality;

    NetworkControlUpdate update;
    update.has_target_rate = true;
    update.target_rate.update_time_ms = feedback.now_ms;
    update.target_rate.target_bitrate_bps = target_bitrate_bps;
    update.target_rate.stable_target_bitrate_bps = stable_target_bitrate_bps_;
    update.target_rate.estimate.update_time_ms = feedback.now_ms;
    update.target_rate.estimate.send_bitrate_bps = feedback.send_bitrate_bps;
    update.target_rate.estimate.rtt_ms = feedback.rtt_ms;
    update.target_rate.estimate.jitter_ms = feedback.jitter_ms;
    update.target_rate.estimate.loss_rate = feedback.loss_rate;
    update.target_rate.estimate.quality = quality;

    if (bwe)
    {
        update.target_rate.estimate.recv_bitrate_bps = bwe->target_bitrate_bps;
        update.target_rate.estimate.bandwidth_bps = bwe->target_bitrate_bps;
    }

    update.has_pacer_config = true;
    update.pacer_config.update_time_ms = feedback.now_ms;
    update.pacer_config.pacing_bitrate_bps = target_bitrate_bps * 12 / 10;
    update.pacer_config.time_window_ms = 40;

    update.enable_nack = true;
    update.enable_fec = quality == NetworkQualityLevel::Bad;
    update.request_key_frame = ShouldRequestKeyFrame(feedback.now_ms, quality);

    return update;
}

bool NetworkController::ShouldRequestKeyFrame(uint64_t now_ms, NetworkQualityLevel quality)
{
    if (quality != NetworkQualityLevel::Bad)
    {
        return false;
    }

    if (last_keyframe_request_ms_ != 0 &&
        now_ms >= last_keyframe_request_ms_ &&
        now_ms - last_keyframe_request_ms_ < keyframe_interval_ms_)
    {
        return false;
    }

    last_keyframe_request_ms_ = now_ms;
    return true;
}

void NetworkController::NotifyNetworkControlUpdate(const NetworkControlUpdate& update)
{
    std::vector<std::shared_ptr<INetworkControlObserver>> snapshot;
    snapshot.reserve(observers_.size());

    for (auto& observer : observers_)
    {
        if (auto locked = observer.lock())
        {
            snapshot.push_back(locked);
        }
    }

    observers_.erase(
        std::remove_if(observers_.begin(), observers_.end(),
                       [](const std::weak_ptr<INetworkControlObserver>& observer) {
                           return observer.expired();
                       }),
        observers_.end());

    for (auto& observer : snapshot)
    {
        observer->OnNetworkControlUpdate(update);
    }
}

}