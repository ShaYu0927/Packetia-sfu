#include "MetricsCalculator.h"

uint64_t MetricsCalculator::CalculateBitrateBps(uint64_t bytes, uint64_t duration_ms)
{
    if (duration_ms == 0)
        return 0;

    return bytes * 8 * 1000 / duration_ms;
}

double MetricsCalculator::CalculateLossRate(uint64_t expected_packets, uint64_t received_packets)
{
    if (expected_packets == 0)
        return 0.0;

    if (received_packets >= expected_packets)
        return 0.0;

    const uint64_t lost_packets = expected_packets - received_packets;

    return static_cast<double>(lost_packets) / static_cast<double>(expected_packets);
}

double MetricsCalculator::CalculateFrameRate(uint64_t frame_count, uint64_t duration_ms)
{
    if (duration_ms == 0)
        return 0.0;

    return static_cast<double>(frame_count) * 1000.0 / static_cast<double>(duration_ms);
}

double MetricsCalculator::CalculateAveragePacketSize(uint64_t bytes, uint64_t packet_count)
{
    if (packet_count == 0)
        return 0.0;

    return static_cast<double>(bytes) / static_cast<double>(packet_count);
}

double MetricsCalculator::JitterToMs(uint32_t jitter, uint32_t clock_rate)
{
    if (clock_rate == 0)
        return 0.0;

    return static_cast<double>(jitter) * 1000.0 / static_cast<double>(clock_rate);
}

double MetricsCalculator::FractionLostToRate(uint8_t fraction_lost)
{
    return static_cast<double>(fraction_lost) / 256.0;
}

double MetricsCalculator::FractionLostToPercent(uint8_t fraction_lost)
{
    return FractionLostToRate(fraction_lost) * 100.0;
}