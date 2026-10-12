#ifndef _REMOTE_BITRATE_CONTROLLER_H_
#define _REMOTE_BITRATE_CONTROLLER_H_

#include <cstdint>
#include "bandwidth_usage.h"

class RemoteBitrateController 
{
public:
    // Rates are bps, loss is [0, 1], time is local monotonic milliseconds.
    BweResult Update(BandwidthUsage usage, uint32_t incoming_bitrate_bps,
                     double loss_rate, int64_t now_ms, uint32_t rtt_ms = 0);
    void SetBitrateConstraints(uint32_t min_bps, uint32_t start_bps, uint32_t max_bps);
    void SetRtt(uint32_t rtt_ms);
    void Reset();
    uint32_t target_bitrate_bps() const { return static_cast<uint32_t>(target_bitrate_bps_); }
    RateControlRegion region() const { return region_; }

private:
    RateControlState state_ = RateControlState::kIncrease;
    double ClampRate(double rate) const;
    void UpdateCapacity(double incoming_bps);
    RateControlRegion region_ = RateControlRegion::kUnknown;
    uint32_t min_bitrate_bps_ = 80000;
    uint32_t start_bitrate_bps_ = 500000;
    uint32_t max_bitrate_bps_ = 2000000;
    double target_bitrate_bps_ = 500000;
    double average_rtt_ms_ = 0;
    double capacity_kbps_ = -1;
    double capacity_variance_ = 0.4;
    double max_hold_rate_ = 0;
    int64_t last_update_ms_ = -1;
    int64_t first_incoming_ms_ = -1;
    uint32_t initial_incoming_bps_ = 0;
    bool initialized_ = false;
    // RTT-aware reduction/observation cycle to separate congestion from base loss.
    double baseline_loss_ = 0;
    double loss_before_probe_ = 0;
    double rate_before_probe_ = 0;
    int64_t loss_probe_start_ms_ = -1;
};

#endif /* _REMOTE_BITRATE_CONTROLLER_H_ */
