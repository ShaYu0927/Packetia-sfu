#pragma once

#include "BandwidthUsage.h"

#include <cstdint>
#include <memory>
#include <optional>

namespace packetia::webrtc_port { class TrendlineEstimator; }

namespace media
{
struct TrendlineObservation
{
    BandwidthUsage usage = BandwidthUsage::kNormal;
    double smoothed_delay_ms = 0;
    double slope = 0;
};

// Adapts Packetia's unsigned receive clock and enum to the vendored estimator.
// The caller supplies group deltas and resets on route changes or long gaps.
class WebRtcTrendlineEstimator
{
public:
    WebRtcTrendlineEstimator();
    ~WebRtcTrendlineEstimator();
    std::optional<TrendlineObservation> Update(uint64_t receive_delta_ms,
                                              uint64_t send_delta_ms,
                                              uint64_t receive_time_ms);
    void Reset();

private:
    std::unique_ptr<packetia::webrtc_port::TrendlineEstimator> estimator_;
    std::optional<uint64_t> receive_origin_ms_;
    uint64_t last_receive_ms_ = 0;
};
}
