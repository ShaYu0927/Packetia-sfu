#ifndef _REMOTE_BITRATE_CONTROLLER_H_
#define _REMOTE_BITRATE_CONTROLLER_H_

#include <cstdint>
#include "bandwidth_usage.h"
#include "loss_based_controller.h"

class RemoteBitrateController 
{
public:
    BweResult Update(BandwidthUsage usage, uint32_t incoming_bitrate_bps, double loss_rate, int64_t now_ms);

private:
    RateControlState state_ = RateControlState::kIncrease;
    LossBasedController loss_controller_;

    uint32_t target_bitrate_bps_ = 500000;
    uint32_t min_bitrate_bps_ = 80000;
    uint32_t max_bitrate_bps_ = 2000000;

    int64_t last_update_ms_ = 0;
};

#endif /* _REMOTE_BITRATE_CONTROLLER_H_ */