#ifndef _ALR_DETECTOR_H_
#define _ALR_DETECTOR_H_

#include <cstdint>
#include <optional>
class AlrDetector
{
public:
    struct Config
    {
        // ALR 使用的目标码率比例
        double bandwidth_usage_ratio = 0.8;

        // budget_ratio 超过该值，进入 ALR
        double start_budget_level_ratio = 0.4;

        // budget_ratio 低于该值，退出 ALR
        double stop_budget_level_ratio = -0.6;

        // budget 最大缓存窗口，单位 ms
        int64_t max_budget_ms = 500;
    };

public:
    explicit AlrDetector(const Config& config);

    // 设置当前估算带宽，单位 bps
    void SetEstimatedBitrateBps(uint64_t bitrate_bps);
    // 每次真正发送数据后调用
    void OnBytesSent(std::size_t bytes_sent, int64_t send_time_ms);
    bool InAlr() const;

    // ALR 开始时间，如果不在 ALR，返回空
    std::optional<int64_t> GetAlrStartTimeMs() const;

    double BudgetRatio() const;
private:
    void IncreaseBudget(int64_t delta_time_ms);
    void UseBudget(size_t bytes_sent);
    void ClampBudget();

};

#endif /* _ALR_DETECTOR_H_ */