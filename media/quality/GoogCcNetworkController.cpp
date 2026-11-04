#include "GoogCcNetworkController.h"

namespace media
{

GoogCcNetworkController::GoogCcNetworkController(const NetworkControllerConfig& config)
    : network_available_(config.network_available), bitrate_(config.bitrate)
{
    policy_.SetBitrateConstraints(config.bitrate);
    delay_bwe_.SetBitrateConstraints(config.bitrate);
}

NetworkControlUpdate GoogCcNetworkController::OnNetworkAvailability(const NetworkAvailability& msg)
{
    if (network_available_ == msg.network_available) return latest_update_;
    network_available_ = msg.network_available;
    if (!network_available_)
    {
        // 清除旧动作，避免网络断开后调用者继续使用旧 Pacer/目标码率。
        latest_update_ = NetworkControlUpdate{};
        stats_.Reset();
        delay_bwe_.Reset();
        policy_ = NetworkController();
        policy_.SetBitrateConstraints(bitrate_);
        latest_feedback_ = WeakNetFeedback{};
        latest_bwe_.reset();
        has_feedback_ = false;
    }
    return latest_update_;
}

NetworkControlUpdate GoogCcNetworkController::OnNetworkRouteChange(const NetworkRouteChange& msg)
{
    stats_.Reset();
    delay_bwe_.SetBitrateConstraints(msg.bitrate);
    bitrate_ = msg.bitrate;
    policy_ = NetworkController();
    policy_.SetBitrateConstraints(msg.bitrate);
    latest_feedback_ = WeakNetFeedback{};
    latest_bwe_.reset();
    latest_feedback_.now_ms = msg.at_time_ms;
    has_feedback_ = false;
    latest_update_ = NetworkControlUpdate{};
    return latest_update_;
}

NetworkControlUpdate GoogCcNetworkController::OnProcessInterval(const ProcessInterval& msg)
{
    // 按当前时间隐藏过期的平滑样本，查询不推进码率算法。
    return GetNetworkState(msg.at_time_ms);
}

NetworkControlUpdate GoogCcNetworkController::OnRoundTripTimeUpdate(const RoundTripTimeUpdate& msg)
{
    if (!network_available_) return EmptyUpdate();
    // RTT 本身不提供吞吐信息；只有已经收到其他反馈后才重新计算策略，
    // 避免启动阶段凭一个 RTT 样本产生没有依据的目标码率。
    latest_feedback_.now_ms = msg.at_time_ms;
    latest_feedback_.rtt_ms = msg.rtt_ms;
    delay_bwe_.SetRtt(msg.rtt_ms);
    stats_.OnRttSample(msg.rtt_ms, msg.at_time_ms);
    if (network_available_ && has_feedback_ && latest_bwe_)
    {
        latest_update_ = policy_.OnBweResultAndFeedback(*latest_bwe_, latest_feedback_);
        latest_update_.target_rate.estimate.smoothed = stats_.GetSnapshot(msg.at_time_ms);
        return latest_update_;
    }
    if (!has_feedback_) return EmptyUpdate();
    latest_update_ = policy_.OnFeedback(latest_feedback_);
    latest_update_.target_rate.estimate.smoothed = stats_.GetSnapshot(msg.at_time_ms);
    return latest_update_;
}

NetworkControlUpdate GoogCcNetworkController::OnTransportPacketsFeedback(const TransportPacketsFeedbackMessage& msg)
{
    if (!network_available_ || msg.feedback.packet_feedbacks.empty())
    {
        return EmptyUpdate();
    }

    const BweResult bwe = delay_bwe_.OnTransportFeedback(msg.feedback);
    latest_bwe_ = bwe;

    latest_feedback_.now_ms = msg.feedback.feedback_time_ms;
    latest_feedback_.receive_bitrate_bps = bwe.incoming_bitrate_bps;
    latest_feedback_.loss_rate = msg.feedback.LossRate();
    latest_feedback_.delay_gradient_ms = bwe.delay_trend_ms;
    has_feedback_ = true;
    stats_.OnTransportFeedback(bwe.incoming_bitrate_bps, latest_feedback_.loss_rate,
        msg.feedback.feedback_time_ms);
    latest_update_ = policy_.OnBweResultAndFeedback(bwe, latest_feedback_);
    latest_update_.target_rate.estimate.smoothed = stats_.GetSnapshot(msg.feedback.feedback_time_ms);
    return latest_update_;
}

NetworkControlUpdate GoogCcNetworkController::OnReceiverFeedback(const WeakNetFeedback& feedback)
{
    // RR、NACK、PLI 等非 TWCC 指标仍沿用原来的弱网控制入口。
    if (!network_available_) return EmptyUpdate();
    latest_feedback_ = feedback;
    has_feedback_ = true;
    delay_bwe_.SetRtt(feedback.rtt_ms);
    // Once RR fallback takes over, an RTT event must not replay old TWCC BWE.
    latest_bwe_.reset();
    stats_.OnFeedback(feedback);
    latest_update_ = policy_.OnFeedback(feedback);
    latest_update_.target_rate.estimate.smoothed = stats_.GetSnapshot(feedback.now_ms);
    return latest_update_;
}

NetworkControlUpdate GoogCcNetworkController::GetNetworkState(uint64_t at_time_ms) const
{
    // 状态查询必须是只读操作，不能推进码率或丢包统计窗口。
    if (!network_available_) return EmptyUpdate();
    auto result = latest_update_;
    if (result.has_target_rate)
        result.target_rate.estimate.smoothed = stats_.GetSnapshot(
            at_time_ms ? at_time_ms : latest_feedback_.now_ms);
    return result;
}

NetworkControlUpdate GoogCcNetworkController::EmptyUpdate() const
{
    return NetworkControlUpdate{};
}

}
