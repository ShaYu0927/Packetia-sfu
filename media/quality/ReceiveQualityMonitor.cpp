#include "ReceiveQualityMonitor.h"
#include "logger.h"

namespace media
{
bool ReceiveQualityMonitor::RegisterSource(uint32_t source_ssrc)
{
    std::lock_guard<std::mutex> lock(mutex_);
    return controllers_.try_emplace(source_ssrc).second;
}

NetworkControlUpdate ReceiveQualityMonitor::OnFeedback(uint32_t source_ssrc, const WeakNetFeedback& feedback)
{
    NetworkControlUpdate update;
    NetworkQualityLevel previous_quality = NetworkQualityLevel::Unknown;
    NetworkQualityLevel current_quality = NetworkQualityLevel::Unknown;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        const auto found = controllers_.find(source_ssrc);
        if (found == controllers_.end()) return update;
        auto& controller = found->second;
        previous_quality = controller.GetSnapshot().quality;
        update = controller.OnFeedback(feedback);
        current_quality = controller.GetSnapshot().quality;
    }

    LOG_DEBUG("[WEAK_NET][CONTROL] update",
             " ssrc=", source_ssrc,
             " quality=", ToString(current_quality),
             " has_target_rate=", update.has_target_rate,
             " target_bitrate_bps=", update.target_rate.target_bitrate_bps,
             " stable_target_bitrate_bps=", update.target_rate.stable_target_bitrate_bps,
             " has_pacer_config=", update.has_pacer_config,
             " pacing_bitrate_bps=", update.pacer_config.pacing_bitrate_bps,
             " enable_nack=", update.enable_nack,
             " enable_fec=", update.enable_fec,
             " request_key_frame=", update.request_key_frame);

    if (current_quality != previous_quality)
    {
        LOG_INFO("[WEAK_NET] quality changed",
                 " ssrc=", source_ssrc,
                 " from=", ToString(previous_quality),
                 " to=", ToString(current_quality),
                 " loss=", feedback.loss_rate,
                 " jitter_ms=", feedback.jitter_ms,
                 " sender_bitrate_bps=", feedback.send_bitrate_bps,
                 " target_bitrate_bps=", update.target_rate.target_bitrate_bps);
    }

    return update;
}

std::optional<NetworkStatsSnapshot> ReceiveQualityMonitor::GetSnapshot(uint32_t source_ssrc, uint64_t now_ms) const
{
    std::lock_guard<std::mutex> lock(mutex_);
    const auto found = controllers_.find(source_ssrc);
    if (found == controllers_.end()) return std::nullopt;
    return found->second.GetSnapshot(now_ms);
}

void ReceiveQualityMonitor::Remove(uint32_t ssrc)
{
    std::lock_guard<std::mutex> lock(mutex_);
    controllers_.erase(ssrc);
}

void ReceiveQualityMonitor::Clear()
{
    std::lock_guard<std::mutex> lock(mutex_);
    controllers_.clear();
}
}
