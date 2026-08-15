#ifndef _METRICS_CALCULATOR_H_
#define _METRICS_CALCULATOR_H_

#include <cstdint>

struct MetricsSnapshot
{
    uint64_t timestamp_ms = 0;             // Snapshot time

    uint64_t rtp_packet_count = 0;         // Total RTP packets
    uint64_t rtp_bytes = 0;                // Total RTP bytes
    uint64_t frame_count = 0;              // Total frames

    uint64_t duplicate_packets = 0;        // Total duplicate packets
    uint64_t out_of_order_packets = 0;     // Total out-of-order packets

    uint64_t expected_packets = 0;          // Expected RTP packets
};

struct CalculatedMetrics
{
    uint64_t bitrate_bps = 0;              // Bitrate in bps

    double packet_rate = 0.0;              // Packets per second
    double frame_rate = 0.0;               // Frames per second

    double packet_loss_rate = 0.0;          // Loss rate [0.0, 1.0]

    uint64_t packet_delta = 0;              // Packets in interval
    uint64_t byte_delta = 0;                // Bytes in interval
    uint64_t frame_delta = 0;               // Frames in interval
};


class MetricsCalculator
{
public:
    /**
     * Calculate bitrate in bits per second.
     */
    static uint64_t CalculateBitrateBps(uint64_t bytes, uint64_t duration_ms);

    /**
     * Calculate packet loss rate in range [0.0, 1.0].
     */
    static double CalculateLossRate(uint64_t expected_packets, uint64_t received_packets);

    /**
     * Calculate frame rate in frames per second.
     */
    static double CalculateFrameRate(uint64_t frame_count, uint64_t duration_ms);

    /**
     * Calculate average RTP packet size in bytes.
     */
    static double CalculateAveragePacketSize(uint64_t bytes, uint64_t packet_count);

    /**
     * Convert RTP jitter timestamp units to milliseconds.
     */
    static double JitterToMs(uint32_t jitter, uint32_t clock_rate);

    /**
     * Convert RTCP fraction lost to loss rate in range [0.0, 1.0].
     */
    static double FractionLostToRate(uint8_t fraction_lost);

    /**
     * Convert RTCP fraction lost to percentage in range [0.0, 100.0].
     */
    static double FractionLostToPercent(uint8_t fraction_lost);
};

#endif /* _METRICS_CALCULATOR_H_ */