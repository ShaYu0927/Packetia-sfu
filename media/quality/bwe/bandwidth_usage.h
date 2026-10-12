#ifndef _BAND_WIDTH_USAGE_H_
#define _BAND_WIDTH_USAGE_H_

#include <cstdint>


enum class BandwidthUsage
{
    kNormal,
    kUnderUsing,
    kOverUsing,
};

enum class RateControlState 
{
    kIncrease,
    kHold,
    kDecrease,
};

enum class RateControlRegion
{
    kUnknown,
    kAboveMax,
    kNearMax,
};

struct BweResult 
{
    uint32_t target_bitrate_bps = 0;                            /* 本次带宽估计输出的目标发送码率，单位 bps */
    BandwidthUsage usage = BandwidthUsage::kNormal;             /* 当前时延检测状态：正常、保持观察或过载 */
    RateControlState rc_state = RateControlState::kIncrease;    /* 当前码率控制状态：升码率、保持或降码率 */
    double delay_trend_ms = 0.0;                                /* 延迟趋势值，单位 ms，用于反映网络排队延迟变化 */
    double loss_rate = 0.0;                                     /* 当前丢包率，取值范围 0.0 ~ 1.0，例如 0.02 表示 2% */
    uint32_t incoming_bitrate_bps = 0; // Measured receive rate, separate from target.
    bool estimate_valid = false;      // False until receive-rate observation initializes the estimate.
    RateControlRegion region = RateControlRegion::kUnknown;
};



#endif /* _BAND_WIDTH_USAGE_H_ */
