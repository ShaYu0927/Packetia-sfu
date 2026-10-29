#ifndef PACKETIA_MEDIA_QUALITY_BWE_DELAY_TREND_DETECTOR_H_
#define PACKETIA_MEDIA_QUALITY_BWE_DELAY_TREND_DETECTOR_H_

#include "BandwidthUsage.h"

class DelayTrendDetector 
{
public:
    BandwidthUsage OnPacket(uint32_t send_ms, int64_t arrival_ms);

    double trend_ms() const { return trend_ms_; }

private:
    bool has_prev_ = false;
    uint32_t prev_send_ms_ = 0;
    int64_t prev_arrival_ms_ = 0;

    double trend_ms_ = 0.0;
    double threshold_ms_ = 25.0;
    int overuse_duration_ms_ = 0;
};


#endif /* PACKETIA_MEDIA_QUALITY_BWE_DELAY_TREND_DETECTOR_H_ */