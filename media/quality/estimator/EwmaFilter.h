#ifndef _EWMAFILTER_H_
#define _EWMAFILTER_H_

#include <cstdint>
#include <optional>


namespace media 
{

class SmoothingFilter
{
public:

    virtual ~SmoothingFilter() = default;


    /**
     * 添加一个采样值
     *
     * sample:
     *   RTT / jitter / loss 等网络指标
     *
     * timestamp_ms:
     *   采样发生时间
     */
    virtual void AddSample(double sample, uint64_t timestamp_ms) = 0;


    /**
     * 获取当前平滑结果
     *
     * now_ms:
     *   当前时间
     *
     * 某些算法需要根据时间衰减
     */
    virtual std::optional<double> GetValue(uint64_t now_ms) const = 0;


    /**
     * 设置时间常数
     *
     * 越大:
     *   越平滑
     *
     * 越小:
     *   越敏感
     */
    virtual bool SetTimeConstantMs(uint32_t time_constant_ms) = 0;
};

class EwmaFilter final : public SmoothingFilter
{
public:

    // 固定权重模式：新结果 = alpha * 旧结果 + (1 - alpha) * 新样本。
    // alpha 必须在 [0, 1]：越大越平滑；0 跟随最新样本，1 保留首个样本。
    explicit EwmaFilter(double alpha);

    // 首个有效样本直接初始化；忽略非有限值及重复、倒退的采样时间。
    void AddSample(double sample, uint64_t timestamp_ms) override;

    // 返回已有估计；未初始化或查询时间早于最后采样时间时返回空值。
    // 查询不更新滤波状态，数据过期由调用方处理。
    std::optional<double> GetValue(uint64_t now_ms) const override;

    // 切换到时间权重模式，旧值权重为 exp(-采样间隔 / 时间常数)。
    // 时间常数为 0 时返回 false，保留原配置。
    bool SetTimeConstantMs(uint32_t time_constant_ms) override;

    // 清除历史采样，保留权重/时间常数配置。
    void Reset();
    std::optional<uint64_t> LastSampleTimeMs() const;

private:

    double alpha_;
    // Unset preserves fixed-alpha mode; time mode uses exp(-elapsed / tau).
    std::optional<uint32_t> time_constant_ms_;
    double value_{0};
    bool initialized_{false};
    uint64_t last_timestamp_ms_{0};
};

}
#endif /* _EWMAFILTER_H_ */
