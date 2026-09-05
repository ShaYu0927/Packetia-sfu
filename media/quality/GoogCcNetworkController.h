#ifndef PACKETIA_MEDIA_QUALITY_GOOG_CC_NETWORK_CONTROLLER_H_
#define PACKETIA_MEDIA_QUALITY_GOOG_CC_NETWORK_CONTROLLER_H_

#include "NetworkController.h"
#include "NetworkControllerInterface.h"
#include "bwe/transport_bwe_controller.h"

namespace media
{

/**
 * GoogCC 形状的编排层。
 *
 * 当前阶段只拆架构，不改变线上算法：
 *   - TransportBweController 负责基于 Transport-CC 的时延/吞吐估计；
 *   - NetworkController 负责质量分级、目标码率和 Pacer 建议。
 *
 * 后续可以逐个加入 ProbeController、AcknowledgedBitrateEstimator、ALR、
 * LossBasedBwe 和 CongestionWindow，而 INetworkController 调用接口保持不变。
 * 本类只负责编排和状态汇合，不负责 RTCP 解析与实际发包。
 */
class GoogCcNetworkController final : public INetworkController
{
public:
    explicit GoogCcNetworkController(const NetworkControllerConfig& config = {});

    NetworkControlUpdate OnNetworkAvailability(const NetworkAvailability& msg) override;
    NetworkControlUpdate OnNetworkRouteChange(const NetworkRouteChange& msg) override;
    NetworkControlUpdate OnProcessInterval(const ProcessInterval& msg) override;
    NetworkControlUpdate OnRoundTripTimeUpdate(const RoundTripTimeUpdate& msg) override;
    NetworkControlUpdate OnTransportPacketsFeedback(const TransportPacketsFeedbackMessage& msg) override;
    NetworkControlUpdate OnReceiverFeedback(const WeakNetFeedback& feedback) override;
    NetworkControlUpdate GetNetworkState(uint64_t at_time_ms) const override;

private:
    // 返回不包含目标码率、Pacer、探测或关键帧请求的空动作。
    NetworkControlUpdate EmptyUpdate() const;

    // 网络不可用时仍可缓存观测值，但禁止输出新的发送控制动作。
    bool network_available_ = true;

    // 最近一次 RR/弱网汇总反馈；TWCC 和 RTT 更新会补充这份状态。
    WeakNetFeedback latest_feedback_;
    bool has_feedback_ = false;

    // 最近一次有效输出，供状态查询和周期处理使用。
    NetworkControlUpdate latest_update_;

    // 当前项目已有的包级时延带宽估计器，未来可替换为 DelayBasedBwe。
    TransportBweController delay_bwe_;

    // 当前项目已有的控制策略层，未来承接 loss based 与 pushback 融合。
    NetworkController policy_;
};

}

#endif /* PACKETIA_MEDIA_QUALITY_GOOG_CC_NETWORK_CONTROLLER_H_ */
