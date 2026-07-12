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
    if (config_.decay_step < 1.0)
    {
        config_.decay_step = 1.0;
    }
}

BandwidthUsage OUTrendDetect::OnPacket(uint32_t send_ms, int64_t arrival_ms, size_t packet_size)
{
    if (!has_prev_)
    {
        has_prev_ = true;
        prev_send_ms_ = send_ms;
        prev_arrival_ms_ = arrival_ms;
        prev_packet_size_ = packet_size;
        return BandwidthUsage::kNormal;
    }

    const int32_t ts_delta_ms = static_cast<int32_t>(send_ms - prev_send_ms_);
    const int64_t t_delta_ms = arrival_ms - prev_arrival_ms_;

    prev_send_ms_ = send_ms;
    prev_arrival_ms_ = arrival_ms;

    if (ts_delta_ms <= 0 || t_delta_ms < 0)
    {
        prev_packet_size_ = packet_size;
        return BandwidthUsage::kNormal;
    }

    const double t_ts_delta_ms = static_cast<double>(t_delta_ms - ts_delta_ms);

    trend_ms_ = trend_ms_ / config_.decay_step + t_ts_delta_ms;
    max_jitter_ms_ = UpdateMaxJitter(arrival_ms, t_ts_delta_ms);
    size_delay_ms_ = EstimateSizeDelay(packet_size);
    effective_trend_ms_ = trend_ms_ - max_jitter_ms_ - size_delay_ms_;

    prev_packet_size_ = packet_size;

    if (effective_trend_ms_ > config_.threshold_ms)
    {
        overuse_duration_ms_ += static_cast<int>(t_delta_ms);
        if (overuse_duration_ms_ >= config_.overuse_duration_ms)
        {
            return BandwidthUsage::kOverUsing;
        }
        return BandwidthUsage::kUnderUsing;
    }

    overuse_duration_ms_ = 0;

    if (effective_trend_ms_ > config_.weak_threshold_ms)
    {
        return BandwidthUsage::kUnderUsing;
    }

    return BandwidthUsage::kNormal;
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
}

double OUTrendDetect::EstimateSizeDelay(size_t packet_size) const
{
    if (config_.size_delay_bitrate_bps == 0 || packet_size <= prev_packet_size_)
    {
        return 0.0;
    }

    const size_t size_delta_bytes = packet_size - prev_packet_size_;
    return static_cast<double>(size_delta_bytes) * 8.0 * 1000.0 /
           static_cast<double>(config_.size_delay_bitrate_bps);
}

double OUTrendDetect::UpdateMaxJitter(int64_t arrival_ms, double delay_delta_ms)
{
    jitter_window_.push_back({arrival_ms, std::fabs(delay_delta_ms)});

    while (!jitter_window_.empty() &&
           arrival_ms - jitter_window_.front().arrival_ms > config_.jitter_window_ms)
    {
        jitter_window_.pop_front();
    }

    double max_jitter_ms = 0.0;
    for (const auto& sample : jitter_window_)
    {
        max_jitter_ms = std::max(max_jitter_ms, sample.delay_delta_ms);
    }

    return max_jitter_ms;
}
