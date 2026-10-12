#include "ou_trend_detect.h"
#include <algorithm>
#include <cmath>

OUTrendDetect::OUTrendDetect()
    : OUTrendDetect(Config())
{
}

OUTrendDetect::OUTrendDetect(const Config& config)
    : config_(config)
{
    config_.threshold_ms = std::max(config_.threshold_ms, 1.0);
    config_.weak_threshold_ms = std::clamp(config_.weak_threshold_ms, 0.0, config_.threshold_ms);
    config_.overuse_duration_ms = std::max(config_.overuse_duration_ms, 1);
    config_.jitter_window_ms = std::max(config_.jitter_window_ms, 1);
    config_.jitter_floor_ms = std::clamp(config_.jitter_floor_ms, 0.0, 550.0);
    bitrate_bps_ = config_.size_delay_bitrate_bps;
}

BandwidthUsage OUTrendDetect::OnPacket(uint32_t send_ms, int64_t arrival_ms, size_t packet_size)
{
    if (arrival_ms < 0) return usage_;
    if (!has_prev_)
    {
        has_prev_ = true;
        prev_send_ms_ = send_ms;
        prev_arrival_ms_ = arrival_ms;
        prev_packet_size_ = packet_size;
        return BandwidthUsage::kNormal;
    }

    const uint32_t ts_delta_ms = send_ms - prev_send_ms_;
    const int64_t t_delta_ms = arrival_ms - prev_arrival_ms_;

    // Signed modular delta handles uint32 send-clock wrap. Late/out-of-order
    // feedback must not move the reference back and corrupt the next sample.
    if (ts_delta_ms > INT32_MAX || t_delta_ms < 0) return usage_;
    prev_send_ms_ = send_ms;
    prev_arrival_ms_ = arrival_ms;

    const double t_ts_delta_ms = static_cast<double>(t_delta_ms - ts_delta_ms);

    max_jitter_ms_ = UpdateMaxJitter(arrival_ms, t_ts_delta_ms);
    size_delay_ms_ = EstimateSizeDelay(packet_size);
    const double duration_ms = std::min<int64_t>(t_delta_ms, 2000);
    const double step = std::pow(trend_ms_ > 0 ? 2.0 : 4.0, duration_ms / 1000.0);
    trend_ms_ = std::clamp(trend_ms_ / step + t_ts_delta_ms,
                          -200.0, 3500.0 + max_jitter_ms_ + size_delay_ms_);
    if (rtt_changed_ && rtt_ms_ > 2500 && bitrate_bps_ > 30000)
        trend_ms_ = std::min(rtt_ms_ / 2.0, 2000.0);
    rtt_changed_ = false;
    effective_trend_ms_ = trend_ms_ - max_jitter_ms_ - size_delay_ms_;

    prev_packet_size_ = packet_size;

    const double sensitivity = region_ == RateControlRegion::kNearMax ? 1.0 : 1.5;
    if (effective_trend_ms_ > config_.threshold_ms * sensitivity)
    {
        overuse_duration_ms_ = static_cast<int>(std::min<int64_t>(
            static_cast<int64_t>(overuse_duration_ms_) + static_cast<int64_t>(duration_ms),
            config_.overuse_duration_ms));
        if (overuse_duration_ms_ >= config_.overuse_duration_ms)
        {
            return usage_ = BandwidthUsage::kOverUsing;
        }
        return usage_ = BandwidthUsage::kUnderUsing;
    }

    overuse_duration_ms_ = 0;

    if (effective_trend_ms_ > config_.weak_threshold_ms)
    {
        return usage_ = BandwidthUsage::kUnderUsing;
    }

    return usage_ = BandwidthUsage::kNormal;
}

void OUTrendDetect::Reset()
{
    has_prev_ = false;
    prev_send_ms_ = 0;
    prev_arrival_ms_ = 0;
    prev_packet_size_ = 0;
    trend_ms_ = 0.0;
    effective_trend_ms_ = 0.0;
    max_jitter_ms_ = 0.0;
    size_delay_ms_ = 0.0;
    overuse_duration_ms_ = 0;
    jitter_window_.clear();
    usage_ = BandwidthUsage::kNormal;
    region_ = RateControlRegion::kUnknown;
    bitrate_bps_ = config_.size_delay_bitrate_bps;
    rtt_ms_ = 0;
    rtt_changed_ = false;
}

void OUTrendDetect::SetNetworkEstimate(uint32_t bitrate_bps, uint32_t rtt_ms)
{
    bitrate_bps_ = bitrate_bps;
    rtt_changed_ = rtt_changed_ || (rtt_ms != 0 && rtt_ms != rtt_ms_);
    if (rtt_ms) rtt_ms_ = rtt_ms;
}

double OUTrendDetect::EstimateSizeDelay(size_t packet_size) const
{
    if (bitrate_bps_ < 10000 || packet_size <= prev_packet_size_)
    {
        return 0.0;
    }

    const size_t size_delta_bytes = packet_size - prev_packet_size_;
    return std::min(1000.0, static_cast<double>(size_delta_bytes) * 8.0 * 1000.0 /
           (static_cast<double>(bitrate_bps_) * 1.5));
}

double OUTrendDetect::UpdateMaxJitter(int64_t arrival_ms, double delay_delta_ms)
{
    const double jitter_ms = std::clamp(delay_delta_ms, config_.jitter_floor_ms, 550.0);
    // Monotonic maximum queue: each sample enters and leaves once, avoiding
    // scanning thousands of samples for every RTP packet.
    while (!jitter_window_.empty() && jitter_window_.back().delay_delta_ms <= jitter_ms)
        jitter_window_.pop_back();
    jitter_window_.push_back({arrival_ms, jitter_ms});

    while (!jitter_window_.empty() &&
           (arrival_ms - jitter_window_.front().arrival_ms > config_.jitter_window_ms ||
            jitter_window_.size() > 2048))
    {
        jitter_window_.pop_front();
    }

    return jitter_window_.front().delay_delta_ms;
}
