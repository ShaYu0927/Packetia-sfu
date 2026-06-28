#include "incoming_bitrate_estimator.h"

uint32_t IncomingBitrateEstimator::Update(size_t packet_size, int64_t now_ms) 
{
    samples_.push_back({now_ms, packet_size});

    while (!samples_.empty() && now_ms - samples_.front().time_ms > window_ms_) 
    {
        samples_.pop_front();
    }

    size_t total_bytes = 0;
    for (const auto& sample : samples_) 
    {
        total_bytes += sample.bytes;
    }

    if (samples_.empty()) 
    {
        return 0;
    }

    return static_cast<uint32_t>(total_bytes * 8 * 1000 / window_ms_);
}