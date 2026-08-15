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

    explicit EwmaFilter(double alpha);
    void AddSample(double sample, uint64_t timestamp_ms) override;
    std::optional<double> GetValue(uint64_t now_ms) const override;
    bool SetTimeConstantMs(uint32_t time_constant_ms) override;


private:

    double alpha_;
    double value_{0};
    bool initialized_{false};
    uint64_t last_timestamp_ms_{0};
};

}
#endif /* _EWMAFILTER_H_ */