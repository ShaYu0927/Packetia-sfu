#ifndef _OU_TREND_DETECT_H_
#define _OU_TREND_DETECT_H_

#include <cstdint>
#include <cstddef>
#include <deque>
#include "bandwidth_usage.h"

class OUTrendDetect
{
public:
    struct Config
    {
        double threshold_ms = 25.0;
        double weak_threshold_ms = 5.0;
        double decay_step = 16.0;
        int overuse_duration_ms = 100;
        int jitter_window_ms = 500;
        uint32_t size_delay_bitrate_bps = 1000 * 1000;
    };

    OUTrendDetect();
    explicit OUTrendDetect(const Config& config);

    BandwidthUsage OnPacket(uint32_t send_ms, int64_t arrival_ms, size_t packet_size);

    void Reset();

    double trend_ms() const { return trend_ms_; }
    double effective_trend_ms() const { return effective_trend_ms_; }
    double max_jitter_ms() const { return max_jitter_ms_; }
    double size_delay_ms() const { return size_delay_ms_; }

private:
    struct JitterSample
    {
        int64_t arrival_ms = 0;
        double delay_delta_ms = 0.0;
    };

    double EstimateSizeDelay(size_t packet_size) const;
    double UpdateMaxJitter(int64_t arrival_ms, double delay_delta_ms);

private:
    Config config_;

    bool has_prev_ = false;
    uint32_t prev_send_ms_ = 0;
    int64_t prev_arrival_ms_ = 0;
    size_t prev_packet_size_ = 0;

    double trend_ms_ = 0.0;
    double effective_trend_ms_ = 0.0;
    double max_jitter_ms_ = 0.0;
    double size_delay_ms_ = 0.0;
    int overuse_duration_ms_ = 0;

    std::deque<JitterSample> jitter_window_;
};

#endif /* _OU_TREND_DETECT_H_ */
