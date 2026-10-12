#include "incoming_bitrate_estimator.h"
#include <algorithm>
#include <limits>

uint32_t IncomingBitrateEstimator::Update(size_t packet_size, int64_t now_ms) 
{
    if (now_ms < 0) return 0;
    if (!samples_.empty() && now_ms < samples_.back().time_ms) return 0;
    // PacketSendInfo uses uint32_t byte sizes; keep this standalone entry point
    // in the same range so the bounded sample sum cannot overflow.
    packet_size = std::min<size_t>(packet_size, std::numeric_limits<uint32_t>::max());
    // Bound storage even if many packets share the same millisecond.
    while (!samples_.empty() && (now_ms - samples_.front().time_ms > window_ms_ || samples_.size() >= 2048)) {
        total_bytes_ -= samples_.front().bytes;
        samples_.pop_front();
    }
    samples_.push_back({now_ms, packet_size});
    total_bytes_ += packet_size;

    // A single startup packet is not a trustworthy receive-rate estimate.
    const int64_t span_ms = now_ms - samples_.front().time_ms;
    if (span_ms < 100) return 0;
    const long double rate = static_cast<long double>(total_bytes_) * 8000 / span_ms;
    return static_cast<uint32_t>(std::min(rate, static_cast<long double>(std::numeric_limits<uint32_t>::max())));
}

void IncomingBitrateEstimator::Reset()
{
    samples_.clear();
    total_bytes_ = 0;
}
