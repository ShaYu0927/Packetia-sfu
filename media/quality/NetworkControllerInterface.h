#ifndef PACKETIA_MEDIA_QUALITY_NETWORK_CONTROLLER_INTERFACE_H_
#define PACKETIA_MEDIA_QUALITY_NETWORK_CONTROLLER_INTERFACE_H_

#include "NetworkStats.h"
#include "WeakNetController.h"

namespace media
{

struct NetworkControllerConfig
{
    // 控制器允许输出的最小、初始和最大目标码率，单位为 bit/s。
    BitrateConstraints bitrate;

    // 创建控制器时网络是否已经可用；不可用时控制器不应产生发送动作。
    bool network_available = true;
};

// 网络连通状态变化。通常由 ICE、DTLS 或底层 Transport 通知。
struct NetworkAvailability
{
    // 本地单调时钟，单位 ms。控制算法内部不得与 Unix 时间混用。
    uint64_t at_time_ms = 0;
    bool network_available = true;
};

// 发送路径发生变化，例如 ICE 切换 candidate pair 或网卡切换。
// 新路径不应继续沿用旧路径积累的带宽、时延趋势和探测状态。
struct NetworkRouteChange
{
    uint64_t at_time_ms = 0;

    // 新路径采用的码率上下界。
    BitrateConstraints bitrate;
};

// 周期驱动消息。后续 Probe、ALR、反馈超时和 Pacer 更新都从这里推进。
struct ProcessInterval
{
    uint64_t at_time_ms = 0;
};

// RTT 更新入口，可来自 RTCP SR/RR，也可以来自其他可靠的传输层测量。
struct RoundTripTimeUpdate
{
    uint64_t at_time_ms = 0;
    uint32_t rtt_ms = 0;
};

// 已完成发送历史匹配的 Transport-CC 反馈。
// RTCP 字节解析和 transport sequence 展开应在进入控制器之前完成。
struct TransportPacketsFeedbackMessage
{
    TransportFeedback feedback;
};

/**
 * 网络控制器的稳定边界，事件模型参考 WebRTC NetworkControllerInterface。
 *
 * 这里有意只使用 Packetia 自己的基础类型，媒体链路因此不需要依赖
 * libwebrtc 的 Environment、Timestamp、DataRate 和 field trial。后续移植
 * GoogCC 子算法时，只需在控制器内部做类型适配，不需要修改上层调用者。
 *
 * 所有 On* 方法返回“本次事件产生的控制动作”。调用方应检查
 * NetworkControlUpdate::HasUpdates()，再应用目标码率、Pacer 或关键帧请求。
 */
class INetworkController
{
public:
    virtual ~INetworkController() = default;

    // 启停网络控制；网络恢复后需要等待新反馈再恢复可信的带宽估计。
    virtual NetworkControlUpdate OnNetworkAvailability(const NetworkAvailability& msg) = 0;

    // 重置旧路由的估计状态并应用新路径约束。
    virtual NetworkControlUpdate OnNetworkRouteChange(const NetworkRouteChange& msg) = 0;

    // 推进不依赖新报文的周期任务。
    virtual NetworkControlUpdate OnProcessInterval(const ProcessInterval& msg) = 0;

    // 更新往返时延并重新评估网络质量。
    virtual NetworkControlUpdate OnRoundTripTimeUpdate(const RoundTripTimeUpdate& msg) = 0;

    // Transport-CC 主入口，用于发送侧基于包级反馈估算可用带宽。
    virtual NetworkControlUpdate OnTransportPacketsFeedback(
        const TransportPacketsFeedbackMessage& msg) = 0;

    // RR/NACK/PLI 等汇总指标入口，保留当前弱网评估链路。
    virtual NetworkControlUpdate OnReceiverFeedback(const WeakNetFeedback& feedback) = 0;

    // 获取当前完整状态快照，不推进算法内部状态。
    virtual NetworkControlUpdate GetNetworkState(uint64_t at_time_ms) const = 0;
};

}

#endif /* PACKETIA_MEDIA_QUALITY_NETWORK_CONTROLLER_INTERFACE_H_ */
