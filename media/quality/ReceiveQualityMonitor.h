#ifndef PACKETIA_MEDIA_QUALITY_RECEIVE_QUALITY_MONITOR_H_
#define PACKETIA_MEDIA_QUALITY_RECEIVE_QUALITY_MONITOR_H_

#include "WeakNetController.h"

#include <cstdint>
#include <mutex>
#include <optional>
#include <unordered_map>

namespace media
{
// 按接收源维护独立的质量控制器；只处理统计数据并返回控制建议。
// 每个端点持有一个实例，SSRC 在本实例内索引，不作为全局轨道身份。
// 不持有端点、RTP 接收器或发送回调，不执行网络操作；内部锁保护评估与清理。
// 接收器建立时显式注册；反馈只更新已注册来源，不创建或恢复已删除的状态。
class ReceiveQualityMonitor
{
public:
    // Returns true for a new source. Duplicate registration keeps existing history.
    bool RegisterSource(uint32_t source_ssrc);
    NetworkControlUpdate OnFeedback(uint32_t source_ssrc, const WeakNetFeedback& feedback);
    std::optional<NetworkStatsSnapshot> GetSnapshot(uint32_t source_ssrc, uint64_t now_ms = 0) const;
    void Remove(uint32_t ssrc);
    void Clear();

private:
    mutable std::mutex mutex_;
    std::unordered_map<uint32_t, WeakNetController> controllers_;
};
}

#endif // PACKETIA_MEDIA_QUALITY_RECEIVE_QUALITY_MONITOR_H_
