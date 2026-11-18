#include "WebRtcTrendlineEstimator.h"
#include "webrtc_trendline/trendline_estimator.h"

#include <limits>

namespace media
{
WebRtcTrendlineEstimator::WebRtcTrendlineEstimator()
    : estimator_(std::make_unique<packetia::webrtc_port::TrendlineEstimator>()) {}

WebRtcTrendlineEstimator::~WebRtcTrendlineEstimator() = default;

void WebRtcTrendlineEstimator::Reset()
{
    estimator_ = std::make_unique<packetia::webrtc_port::TrendlineEstimator>();
    receive_origin_ms_.reset();
    last_receive_ms_ = 0;
}

std::optional<TrendlineObservation> WebRtcTrendlineEstimator::Update(uint64_t receive_delta_ms, uint64_t send_delta_ms, uint64_t receive_time_ms)
{
    if (!receive_origin_ms_) receive_origin_ms_ = receive_time_ms;
    // Rebase before signed conversion; absolute remote clocks may exceed INT64_MAX.
    // A backwards clock or an unrepresentable span requires a new observation window.
    if (receive_time_ms < *receive_origin_ms_ || receive_time_ms < last_receive_ms_ ||
        receive_time_ms - *receive_origin_ms_ > static_cast<uint64_t>(std::numeric_limits<int64_t>::max()))
    {
        Reset();
        return std::nullopt;
    }
    last_receive_ms_ = receive_time_ms;
    const auto relative_receive_ms = static_cast<int64_t>(receive_time_ms - *receive_origin_ms_);
    // With no network predictor, the upstream default path ignores send_time_ms
    // and packet_size. Feed group deltas once; no smoothing happens in this adapter.
    estimator_->Update(static_cast<double>(receive_delta_ms), static_cast<double>(send_delta_ms),
                       0, relative_receive_ms, 0, true);
    // Preserve Packetia's contract: no group decision before a full 20-sample window.
    if (!estimator_->HasFullWindow()) return std::nullopt;

    BandwidthUsage usage = BandwidthUsage::kNormal;
    switch (estimator_->State())
    {
        case packetia::webrtc_port::BandwidthUsage::kBwOverusing:
            usage = BandwidthUsage::kOverUsing;
            break;
        case packetia::webrtc_port::BandwidthUsage::kBwUnderusing:
            usage = BandwidthUsage::kUnderUsing;
            break;
        default:
            break;
    }
    return TrendlineObservation{usage, estimator_->SmoothedDelayMs(), estimator_->Slope()};
}
}
