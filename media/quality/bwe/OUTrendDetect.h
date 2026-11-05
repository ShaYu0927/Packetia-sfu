#ifndef PACKETIA_MEDIA_QUALITY_BWE_OU_TREND_DETECT_H_
#define PACKETIA_MEDIA_QUALITY_BWE_OU_TREND_DETECT_H_

#include <cstdint>
#include <cstddef>
#include <deque>
#include "BandwidthUsage.h"

/*
 * 根据数据包发送间隔与到达间隔的变化，检测网络带宽使用状态。
 * 结合时延趋势、网络抖动及包大小引入的时延影响，为码率控制提供判断依据。
 * 本类负责状态检测，不直接调整发送码率。
 */
class OUTrendDetect
{
public:
    struct Config
    {
        /* 时延趋势判断阈值，单位：毫秒。 */
        double threshold_ms = 35.0;

        /* 较低的时延趋势判断阈值，具体启用条件由实现决定，单位：毫秒。 */
        double weak_threshold_ms = 7.0;

        /* 过载判断所需的持续时间，单位：毫秒。 */
        int overuse_duration_ms = 35;

        /* 抖动样本的保留窗口，单位：毫秒。 */
        int jitter_window_ms = 10500;

        /* 抖动估计下限，单位：毫秒。 */
        double jitter_floor_ms = 30.0;

        /* 估算包大小所引入传输时延时使用的基准码率，单位：bit/s。 */
        uint32_t size_delay_bitrate_bps = 1000 * 1000;
    };

    /* 使用默认配置创建检测器。 */
    OUTrendDetect();

    /* 使用指定配置创建检测器。 */
    explicit OUTrendDetect(const Config& config);

    /*
     * 输入一个数据包的时间与大小信息，更新并返回网络带宽使用状态。
     * send_ms：发送时间，单位为毫秒，使用发送端时钟。
     * arrival_ms：到达时间，单位为毫秒，使用接收端时钟。
     * packet_size：包大小，单位为字节。
     * 同一方向的时间戳应保持一致的时钟来源。
     */
    BandwidthUsage OnPacket(uint32_t send_ms, int64_t arrival_ms, size_t packet_size);

    /* 重置检测状态；具体重置范围由实现决定。 */
    void Reset();

    /* 更新网络估计，bitrate_bps 单位为 bit/s，rtt_ms 单位为毫秒。 */
    void SetNetworkEstimate(uint32_t bitrate_bps, uint32_t rtt_ms);

    /* 更新码率控制区域，供后续检测判断使用。 */
    void SetRateControlRegion(RateControlRegion region) { region_ = region; }

    /* 获取当前时延趋势，单位：毫秒。 */
    double trend_ms() const { return trend_ms_; }

    /* 获取用于状态判断的有效时延趋势，单位：毫秒。 */
    double effective_trend_ms() const { return effective_trend_ms_; }

    /* 获取当前窗口的最大抖动估计，单位：毫秒。 */
    double max_jitter_ms() const { return max_jitter_ms_; }

    /* 获取包大小引入的时延估计，单位：毫秒。 */
    double size_delay_ms() const { return size_delay_ms_; }

private:
    /* 抖动窗口内的单个时延变化样本。 */
    struct JitterSample
    {
        /* 样本对应的数据包到达时间，用于窗口过期判断，单位：毫秒。 */
        int64_t arrival_ms = 0;

        /* 数据包的时延变化量，单位：毫秒。 */
        double delay_delta_ms = 0.0;
    };

    /* 根据包大小估算传输时延影响，返回值单位为毫秒。 */
    double EstimateSizeDelay(size_t packet_size) const;

    /* 更新抖动窗口并返回最大抖动估计，时间参数及返回值单位均为毫秒。 */
    double UpdateMaxJitter(int64_t arrival_ms, double delay_delta_ms);

private:
    /* 检测器配置。 */
    Config config_;

    /* 是否已记录前一个数据包。 */
    bool has_prev_ = false;

    /* 前一个数据包的发送时间，单位：毫秒。 */
    uint32_t prev_send_ms_ = 0;

    /* 前一个数据包的到达时间，单位：毫秒。 */
    int64_t prev_arrival_ms_ = 0;

    /* 前一个数据包的大小，单位：字节。 */
    size_t prev_packet_size_ = 0;

    /* 当前时延趋势，单位：毫秒。 */
    double trend_ms_ = 0.0;

    /* 经过修正、用于状态判断的有效时延趋势，单位：毫秒。 */
    double effective_trend_ms_ = 0.0;

    /* 当前窗口内的最大抖动估计，单位：毫秒。 */
    double max_jitter_ms_ = 0.0;

    /* 包大小引入的时延估计，单位：毫秒。 */
    double size_delay_ms_ = 0.0;

    /* 当前过载累计持续时间，单位：毫秒。 */
    int overuse_duration_ms_ = 0;

    /* 当前检测到的网络带宽使用状态。 */
    BandwidthUsage usage_ = BandwidthUsage::kNormal;

    /* 当前码率控制区域。 */
    RateControlRegion region_ = RateControlRegion::kUnknown;

    /* 当前网络码率估计，单位：bit/s。 */
    uint32_t bitrate_bps_ = 1000 * 1000;

    /* 当前往返时延估计，单位：毫秒。 */
    uint32_t rtt_ms_ = 0;

    /* RTT 估计是否发生变化，供后续检测处理使用。 */
    bool rtt_changed_ = false;

    /* 按到达时间维护的抖动样本窗口。 */
    std::deque<JitterSample> jitter_window_;
};

#endif /* PACKETIA_MEDIA_QUALITY_BWE_OU_TREND_DETECT_H_ */