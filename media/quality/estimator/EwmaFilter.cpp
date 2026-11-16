#include "EwmaFilter.h"
#include <cmath>
#include <stdexcept>


namespace media
{

EwmaFilter::EwmaFilter(double alpha)
    : alpha_(alpha)
{
    if (!std::isfinite(alpha) || alpha < 0.0 || alpha > 1.0)
        throw std::invalid_argument("EWMA alpha must be in [0, 1]");
}

void EwmaFilter::AddSample(double sample, uint64_t timestamp_ms)
{
    if (!std::isfinite(sample)) return;
    if (initialized_ && timestamp_ms <= last_timestamp_ms_) return;
    if (!initialized_)
    {
        value_ = sample;
        initialized_ = true;
        last_timestamp_ms_ = timestamp_ms;
        return;
    }

    double old_weight = alpha_;
    double sample_weight = 1.0 - alpha_;
    if (time_constant_ms_)
    {
        const double elapsed_ms = static_cast<double>(timestamp_ms - last_timestamp_ms_);
        const double exponent = -elapsed_ms / *time_constant_ms_;
        old_weight = std::exp(exponent);
        // 小采样间隔时避免 1 - exp(exponent) 的相减精度损失。
        sample_weight = -std::expm1(exponent);
    }
    // 直接使用旧值权重，避免 1 - (1 - alpha) 将很小的 alpha 舍入成 0。
    value_ = old_weight * value_ + sample_weight * sample;
    last_timestamp_ms_ = timestamp_ms;
}

std::optional<double> EwmaFilter::GetValue(uint64_t now_ms) const
{
    if (!initialized_ || now_ms < last_timestamp_ms_)
    {
        return std::nullopt;
    }
    return value_;
}

bool EwmaFilter::SetTimeConstantMs(uint32_t time_constant_ms)
{
    if (time_constant_ms == 0)
    {
        return false;
    }
    time_constant_ms_ = time_constant_ms;
    return true;
}

void EwmaFilter::Reset()
{
    initialized_ = false;
    value_ = 0.0;
    last_timestamp_ms_ = 0;
}

std::optional<uint64_t> EwmaFilter::LastSampleTimeMs() const
{
    return initialized_ ? std::optional<uint64_t>(last_timestamp_ms_) : std::nullopt;
}

}
