#pragma once
#include "WebRtcServiceStats.h"
#include "RoomControl.h"
#include <atomic>
#include <memory>

class TaskScheduler;
namespace server::signaling
{
class RoomSignalHandler;
// 一组房间共用的执行上下文；每个房间固定归属一个 owner。
struct RoomOwner
{
    // 串行执行所属房间的状态变更。
    std::shared_ptr<TaskScheduler> executor;
    // 处理所属房间的信令、成员和媒体会话业务。
    std::shared_ptr<RoomSignalHandler> handler;
    // 当前 owner 的统计数据，在 executor 上读写。
    std::shared_ptr<WebRtcServiceStats> stats;
    // 有界管理命令通道，将后台请求投递到 executor 执行。
    std::shared_ptr<RoomControlChannel> controls;
};

// 所有房间 owner 共用服务级额度，避免分片后把最大会话数放大。
class RoomCapacity
{
public:
    explicit RoomCapacity(std::size_t limit) : limit_(limit) {}
    bool AcquireParticipant() { return Acquire(participants_); }
    bool AcquireMedia() { return Acquire(media_); }
    void ReleaseParticipant() { --participants_; }
    void ReleaseMedia() { --media_; }
private:
    bool Acquire(std::atomic<std::size_t>& count)
    {
        auto value = count.load();
        while (value < limit_)
            if (count.compare_exchange_weak(value, value + 1)) return true;
        return false;
    }
    const std::size_t limit_;
    std::atomic<std::size_t> participants_{0}, media_{0};
};
}
