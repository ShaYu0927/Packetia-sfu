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
    if(!initialized_)
    {
        value_ = sample;
        initialized_ = true;
        last_timestamp_ms_ = timestamp_ms;
        return;
    }

    const double weight = time_constant_ms_ ?
        -std::expm1(-static_cast<double>(timestamp_ms - last_timestamp_ms_) / *time_constant_ms_) :
        1.0 - alpha_;
    value_ = (1.0 - weight) * value_ + weight * sample;
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
    if(time_constant_ms == 0)
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
