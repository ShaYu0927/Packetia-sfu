#include "delay_trend_detector.h"

BandwidthUsage DelayTrendDetector::OnPacket(uint32_t send_ms, int64_t arrival_ms) 
{
    if (!has_prev_) 
    {
        has_prev_ = true;
        prev_send_ms_ = send_ms;
        prev_arrival_ms_ = arrival_ms;
        return BandwidthUsage::kNormal;
    }

    int32_t send_delta_ms = static_cast<int32_t>(send_ms - prev_send_ms_);
    int64_t arrival_delta_ms = arrival_ms - prev_arrival_ms_;

    prev_send_ms_ = send_ms;
    prev_arrival_ms_ = arrival_ms;

    if (send_delta_ms <= 0 || arrival_delta_ms < 0) 
    {
        return BandwidthUsage::kNormal;
    }

    double delay_delta_ms = static_cast<double>(arrival_delta_ms - send_delta_ms);

    trend_ms_ = trend_ms_ * 0.9 + delay_delta_ms * 0.1;

    if (trend_ms_ > threshold_ms_) 
    {
        overuse_duration_ms_ += static_cast<int>(arrival_delta_ms);
        if (overuse_duration_ms_ >= 100) 
        {
            return BandwidthUsage::kOverUsing;
        }
        return BandwidthUsage::kUnderUsing;
    }

    overuse_duration_ms_ = 0;

    if (trend_ms_ < -threshold_ms_) 
    {
        return BandwidthUsage::kUnderUsing;
    }

    return BandwidthUsage::kNormal;
}