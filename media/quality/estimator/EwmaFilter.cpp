#include "EwmaFilter.h"
#include <cmath>


namespace media
{

EwmaFilter::EwmaFilter(double alpha)
    : alpha_(alpha)
{
}

void EwmaFilter::AddSample(double sample, uint64_t timestamp_ms)
{
    if(!initialized_)
    {
        value_ = sample;
        initialized_ = true;
        last_timestamp_ms_ = timestamp_ms;
        return;
    }

    value_  = alpha_ * value_ + (1.0 - alpha_) * sample;
    last_timestamp_ms_ = timestamp_ms;
}

std::optional<double> EwmaFilter::GetValue(uint64_t now_ms) const
{
    if (!initialized_)
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
    /*
       alpha = exp(-1/T)
    */

    alpha_ = std::exp(-1.0 / time_constant_ms);
    return true;
}

}