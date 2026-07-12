#ifndef _WEAK_NET_CONTROLLER_H_
#define _WEAK_NET_CONTROLLER_H_

#include <cstdint>
#include "NetworkStats.h"

namespace media {

struct WeakNetFeedback
{
    uint64_t now_ms = 0;

    uint32_t send_bitrate_bps = 0;
    uint32_t target_bitrate_bps = 0;

    double loss_rate = 0.0;      // 0.05 = 5%
    uint32_t rtt_ms = 0;
    uint32_t jitter_ms = 0;

    uint64_t nack_count = 0;
    uint64_t pli_count = 0;
    uint64_t fir_count = 0;
};


class WeakNetController
{
public:
    WeakNetController();

    void SetBitrateConstraints(const BitrateConstraints& constraints);

    NetworkControlUpdate OnFeedback(const WeakNetFeedback& feedback);

    NetworkStatsSnapshot GetSnapshot() const { return snapshot_; }

private:
    NetworkQualityLevel EstimateQuality(const WeakNetFeedback& feedback) const;

    uint32_t UpdateTargetBitrate(const WeakNetFeedback& feedback, NetworkQualityLevel quality);

    bool ShouldRequestKeyFrame(const WeakNetFeedback& feedback, NetworkQualityLevel quality);

private:
    BitrateConstraints bitrate_;

    uint32_t target_bitrate_bps_ = 800 * 1000;
    uint32_t stable_target_bitrate_bps_ = 800 * 1000;

    uint64_t last_keyframe_request_ms_ = 0;
    uint64_t keyframe_interval_ms_ = 1000;

    NetworkStatsSnapshot snapshot_;
};

}

#endif /* _WEAK_NET_CONTROLLER_H_ */