#ifndef PACKETIA_KEY_FRAME_RETRY_POLICY_H_
#define PACKETIA_KEY_FRAME_RETRY_POLICY_H_

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <optional>

namespace media
{
// Compute the keyframe request retry interval in milliseconds without sending
// requests or changing state. Use a valid RTT between the SFU and the publisher;
// pass nullopt for unknown or expired estimates. Do not substitute subscriber RTT.
inline uint64_t ComputeKeyFrameRetryIntervalMs(std::optional<double> publisher_rtt_ms)
{
    constexpr uint64_t kFallbackIntervalMs = 1000;
    constexpr uint64_t kMinIntervalMs = 200;
    constexpr uint64_t kMaxIntervalMs = 1000;

    // Use a conservative one-second fallback for missing or invalid estimates.
    if (!publisher_rtt_ms || !std::isfinite(*publisher_rtt_ms) || *publisher_rtt_ms <= 0)
        return kFallbackIntervalMs;

    // Check the upper bound before doubling to avoid overflow for large finite RTTs.
    if (*publisher_rtt_ms >= kMaxIntervalMs / 2.0)
        return kMaxIntervalMs;

    // For valid samples: interval = clamp(2 * RTT, 200 ms, 1000 ms).
    // Round up so integer conversion cannot schedule a retry before this interval.
    const double interval_ms = std::clamp(*publisher_rtt_ms * 2.0,
        static_cast<double>(kMinIntervalMs), static_cast<double>(kMaxIntervalMs));
    return static_cast<uint64_t>(std::ceil(interval_ms));
}
}

#endif
