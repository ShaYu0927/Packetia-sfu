#ifndef _WEAK_NET_CONTROLLER_H_
#define _WEAK_NET_CONTROLLER_H_

#include <cstdint>
#include "NetworkStats.h"

namespace media {

struct WeakNetFeedback
{
    uint64_t now_ms = 0;

    // Bitrate
    uint32_t send_bitrate_bps = 0;
    uint32_t receive_bitrate_bps = 0;   // Rr(ti), measured over last 500 ms
    uint32_t target_bitrate_bps = 0;    // Ar(ti)

    // Delay-based congestion control
    double delay_gradient_ms = 0.0;     // dm(ti)
    double filtered_delay_gradient_ms = 0.0; // m(ti)
    double overuse_threshold_ms = 0.0;  // gamma(ti)

    // Network quality
    double loss_rate = 0.0;             // 0.05 = 5%
    uint32_t rtt_ms = 0;
    uint32_t jitter_ms = 0;

    // RTCP feedback
    uint64_t nack_count = 0;
    uint64_t pli_count = 0;
    uint64_t fir_count = 0;
};

static constexpr double kStateNoiseVariance = 1e-3;  // Q
static constexpr double kNoiseBeta          = 0.95;  // beta
static constexpr double kInitialErrorVar    = 1e-1;  // P(0)

class WeakNetController
{
public:
    WeakNetController() = default;

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
