#ifndef PACKETIA_MEDIA_QUALITY_GOOG_CC_NETWORK_CONTROLLER_H_
#define PACKETIA_MEDIA_QUALITY_GOOG_CC_NETWORK_CONTROLLER_H_

#include "NetworkController.h"
#include "NetworkControllerInterface.h"
#include "TransportBweController.h"
#include <optional>

namespace media
{

/**
 * GoogCC 形状的编排层。
 *
 *   - TransportBweController 负责基于 Transport-CC 的 OU 时延趋势、
 *     接收速率和时间/RTT 驱动的 RemoteBitrateController；
 *   - NetworkController 负责质量分级、目标码率和 Pacer 建议。
 *
 *   - PacketGroupObserver 与 ProbeController 提供可替换算法接口，默认尚无新算法。
 * 后续可以逐个加入 AcknowledgedBitrateEstimator、ALR、
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
    ProbeDecision GetProbeDecision(const ProbeRequest& request);
    void OnProbeEvent(const ProbeEvent& event);
    uint64_t ProbeGeneration() const { return probe_generation_; }

private:
    // 返回不包含目标码率、Pacer、探测或关键帧请求的空动作。
    NetworkControlUpdate EmptyUpdate() const;
    void ExpireFeedback(uint64_t now_ms);
    void ResetFeedback();

    // 网络不可用时不推进估计，恢复后等待新路径反馈。
    bool network_available_ = true;
    BitrateConstraints bitrate_;
    uint64_t feedback_timeout_ms_;
    std::optional<uint64_t> last_feedback_time_ms_;
    std::optional<uint64_t> last_twcc_time_ms_;

    // 最近一次 RR/弱网汇总反馈；TWCC 和 RTT 更新会补充这份状态。
    WeakNetFeedback latest_feedback_;
    bool has_feedback_ = false;

    // 最近一次有效输出，供状态查询和周期处理使用。
    NetworkControlUpdate latest_update_;

    // 当前项目已有的包级时延带宽估计器，未来可替换为 DelayBasedBwe。
    TransportBweController delay_bwe_;
    std::optional<BweResult> latest_bwe_;

    // 当前项目已有的控制策略层，未来承接 loss based 与 pushback 融合。
    NetworkController policy_;
    NetworkStatsCollector stats_;
    std::unique_ptr<IProbeController> probe_controller_;
    uint64_t probe_generation_ = 1;
};

}

#endif /* PACKETIA_MEDIA_QUALITY_GOOG_CC_NETWORK_CONTROLLER_H_ */
