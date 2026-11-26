#include "OUTrendDetect.h"
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
    config_.max_sample_gap_ms = std::max(config_.max_sample_gap_ms, 1);
    config_.min_overuse_samples = std::max(config_.min_overuse_samples, 2);
    bitrate_bps_ = config_.size_delay_bitrate_bps;
}

BandwidthUsage OUTrendDetect::OnPacket(uint32_t send_ms, int64_t arrival_ms, size_t packet_size)
{
    if (arrival_ms < 0)
    {
        return usage_;
    }

    /* 建立参考,不产生时延判断  */
    const auto establish_reference = [&]() {
        has_prev_ = true;
        prev_send_ms_ = send_ms;
        prev_arrival_ms_ = arrival_ms;
        prev_packet_size_ = packet_size;

        usage_ = BandwidthUsage::kNormal;
        return usage_;
    };

    if (!has_prev_)
    {
        return establish_reference();
    }

    /* 无符号相减可以处理 uint32 时间戳的正常回绕。*/
    const uint32_t send_delta_ms = send_ms - prev_send_ms_;
    const int64_t arrival_delta_ms = arrival_ms - prev_arrival_ms_;

    // Reordered samples must not change the reference or clear an active signal.
    if (send_delta_ms > INT32_MAX || arrival_delta_ms < 0)
    {
        return usage_;
    }

    if (send_delta_ms > static_cast<uint32_t>(config_.max_sample_gap_ms) ||
        arrival_delta_ms > config_.max_sample_gap_ms)
    {
        ResetObservation();
        return establish_reference();
    }

    const double delay_delta_ms = static_cast<double>(arrival_delta_ms) - static_cast<double>(send_delta_ms);

    /* 保留现有抖动 包大小补偿和趋势判断 */
    max_jitter_ms_ = UpdateMaxJitter(arrival_ms, delay_delta_ms);
    size_delay_ms_ = EstimateSizeDelay(packet_size);

    const double elapsed_ms = static_cast<double>(arrival_delta_ms);
    const double step = std::pow(
        trend_ms_ > 0 ? 2.0 : 4.0,
        elapsed_ms / 1000.0);

    trend_ms_ = std::clamp(
        trend_ms_ / step + delay_delta_ms,
        -200.0,
        3500.0 + max_jitter_ms_ + size_delay_ms_);

    // 暂时保留原有 RTT 修正，避免同时改变其他策略。
    if (rtt_changed_ && rtt_ms_ > 2500 && bitrate_bps_ > 30000)
        trend_ms_ = std::min(rtt_ms_ / 2.0, 2000.0);

    rtt_changed_ = false;

    effective_trend_ms_ =
        trend_ms_ - max_jitter_ms_ - size_delay_ms_;

    // 完成本次计算后，再更新上一包信息。
    prev_send_ms_ = send_ms;
    prev_arrival_ms_ = arrival_ms;
    prev_packet_size_ = packet_size;

    const double threshold_scale =
        region_ == RateControlRegion::kNearMax ? 1.0 : 1.5;

    const double overuse_threshold_ms =
        config_.threshold_ms * threshold_scale;

    if (effective_trend_ms_ > overuse_threshold_ms)
    {
        if (!overuse_start_ms_)
        {
            // 首次超阈值，从当前时刻开始怀疑。
            // 不追溯到上一包的接收时间。
            overuse_start_ms_ = arrival_ms;
            overuse_samples_ = 1;
        }
        else if (arrival_delta_ms > 0 &&
                 overuse_samples_ < config_.min_overuse_samples)
        {
            // 同一接收毫秒内的多个包不重复增加确认样本数。
            // 达到要求后停止递增，避免计数无限增长。
            ++overuse_samples_;
        }

        const int64_t overuse_elapsed_ms =
            arrival_ms - *overuse_start_ms_;

        const bool enough_time =
            overuse_elapsed_ms >= config_.overuse_duration_ms;

        const bool enough_samples =
            overuse_samples_ >= config_.min_overuse_samples;

        if (enough_time && enough_samples)
            return usage_ = BandwidthUsage::kOverUsing;

        // 沿用你的现有策略：确认期间输出 UnderUsing，
        // 下游码率控制器据此保持观察。
        return usage_ = BandwidthUsage::kUnderUsing;
    }

    // 不再超过过载阈值，本次连续过载候选结束。
    overuse_start_ms_.reset();
    overuse_samples_ = 0;

    if (effective_trend_ms_ > config_.weak_threshold_ms)
        return usage_ = BandwidthUsage::kUnderUsing;

    return usage_ = BandwidthUsage::kNormal;
}

void OUTrendDetect::ResetObservation()
{
    has_prev_ = false;
    prev_send_ms_ = 0;
    prev_arrival_ms_ = 0;
    prev_packet_size_ = 0;
    trend_ms_ = 0.0;
    effective_trend_ms_ = 0.0;
    max_jitter_ms_ = 0.0;
    size_delay_ms_ = 0.0;
    overuse_start_ms_.reset();
    overuse_samples_ = 0;
    jitter_peak_ms_ = 0.0;
    last_jitter_update_ms_.reset();
    jitter_window_.clear();
    usage_ = BandwidthUsage::kNormal;
}

void OUTrendDetect::Reset()
{
    ResetObservation();
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
    if(arrival_ms < 0|| !std::isfinite(delay_delta_ms) || (last_jitter_update_ms_ && arrival_ms <= *last_jitter_update_ms_))
    {
        return std::max(config_.jitter_floor_ms, jitter_peak_ms_);
    }

    const double sample_ms = std::clamp(delay_delta_ms,0.0, 550.0);

    if(!last_jitter_update_ms_)
    {
        jitter_peak_ms_ = sample_ms;
    }
    else
    {
        const double elapsed_ms = static_cast<double>(arrival_ms - *last_jitter_update_ms_);
        const double decay = std::exp(-elapsed_ms / config_.jitter_window_ms);
        jitter_peak_ms_ = std::max(sample_ms, jitter_peak_ms_ * decay);
    }

    last_jitter_update_ms_ = arrival_ms;
    return std::max(config_.jitter_floor_ms, jitter_peak_ms_);  /* 下限补偿值 */
}
