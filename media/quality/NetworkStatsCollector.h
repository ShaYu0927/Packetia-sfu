#ifndef PACKETIA_NETWORK_STATS_COLLECTOR_H_
#define PACKETIA_NETWORK_STATS_COLLECTOR_H_

#include "NetworkStats.h"
#include "estimator/EwmaFilter.h"

namespace media
{
struct WeakNetFeedback;

// Owned by one transport controller or receiving SSRC, under its owner's lock.
// Only actual samples enter the filters: RTT events cannot refresh throughput.
class NetworkStatsCollector
{
public:
    static constexpr uint32_t kTimeConstantMs = 1000;
    static constexpr uint64_t kMaxSampleAgeMs = 3000;

    NetworkStatsCollector();
    void OnFeedback(const WeakNetFeedback& feedback);
    void OnTransportFeedback(uint32_t recv_bitrate_bps, double loss_rate, uint64_t now_ms);
    void OnRttSample(uint32_t rtt_ms, uint64_t now_ms);
    SmoothedNetworkMetrics GetSnapshot(uint64_t now_ms) const;
    void Reset();

private:
    EwmaFilter send_rate_{0.0}, recv_rate_{0.0}, rtt_{0.0}, jitter_{0.0}, loss_{0.0};
};
}
#endif
