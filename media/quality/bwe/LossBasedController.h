#ifndef PACKETIA_MEDIA_QUALITY_BWE_LOSS_BASED_CONTROLLER_H_
#define PACKETIA_MEDIA_QUALITY_BWE_LOSS_BASED_CONTROLLER_H_

#include <cstdint>

/*
    loss < 2%       => bitrate * 1.05
    2% ~ 10%        => hold
    loss > 10%      => bitrate * (1 - 0.5 * loss)
*/

class LossBasedController 
{
public:
    uint32_t Update(uint32_t bitrate_bps, double loss_rate);
};



#endif /* PACKETIA_MEDIA_QUALITY_BWE_LOSS_BASED_CONTROLLER_H_ */