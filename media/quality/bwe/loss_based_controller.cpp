#include "loss_based_controller.h"
#include <algorithm>


uint32_t LossBasedController::Update(uint32_t bitrate_bps, double loss_rate) 
{
    if (loss_rate < 0.02) 
    {
        return static_cast<uint32_t>(bitrate_bps * 1.05);
    }

    if (loss_rate <= 0.10) 
    {
        return bitrate_bps;
    }

    double factor = 1.0 - 0.5 * loss_rate;
    factor = std::max(0.5, factor);

    return static_cast<uint32_t>(bitrate_bps * factor);
}