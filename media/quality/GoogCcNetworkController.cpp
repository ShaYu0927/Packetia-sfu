#include "GoogCcNetworkController.h"
#include <stdexcept>

namespace media
{

GoogCcNetworkController::GoogCcNetworkController(const NetworkControllerConfig& config)
    : network_available_(config.network_available), bitrate_(config.bitrate),
      feedback_timeout_ms_(config.feedback_timeout_ms),
      delay_bwe_(config.packet_group_observer_factory ? config.packet_group_observer_factory() : nullptr),
      probe_controller_(config.probe_controller_factory ? config.probe_controller_factory() : nullptr)
{
    if (!probe_controller_) probe_controller_ = std::make_unique<ProbeController>();
    if (!feedback_timeout_ms_) throw std::invalid_argument("feedback timeout must be positive");
    policy_.SetBitrateConstraints(config.bitrate);
    delay_bwe_.SetBitrateConstraints(config.bitrate);
}

NetworkControlUpdate GoogCcNetworkController::OnNetworkAvailability(const NetworkAvailability& msg)
{
    if (network_available_ == msg.network_available) return latest_update_;
    network_available_ = msg.network_available;
    if (!network_available_)
    {
        ResetFeedback();
    }
    return latest_update_;
}

NetworkControlUpdate GoogCcNetworkController::OnNetworkRouteChange(const NetworkRouteChange& msg)
{
    bitrate_ = msg.bitrate;
    delay_bwe_.SetBitrateConstraints(bitrate_);
    ResetFeedback();
    return latest_update_;
}

void GoogCcNetworkController::ResetFeedback()
{
    probe_controller_->Reset();
    ++probe_generation_;
    stats_.Reset();
    delay_bwe_.Reset();
    policy_ = NetworkController();
    policy_.SetBitrateConstraints(bitrate_);
    latest_feedback_ = WeakNetFeedback{};
    latest_bwe_.reset();
    has_feedback_ = false;
    last_feedback_time_ms_.reset();
    last_twcc_time_ms_.reset();
    latest_update_ = NetworkControlUpdate{};
}

void GoogCcNetworkController::ExpireFeedback(uint64_t now_ms)
{
    const auto expired = [this, now_ms](const std::optional<uint64_t>& at) {
        return at && now_ms >= *at && now_ms - *at >= feedback_timeout_ms_;
    };
    if (expired(last_feedback_time_ms_))
        ResetFeedback();
    else if (expired(last_twcc_time_ms_))
    {
        // RR can stay fresh while TWCC is absent. Do not join the next TWCC
        // sample to old receive-rate windows or old delay/rate-control history.
        delay_bwe_.Reset();
        delay_bwe_.SetRtt(latest_feedback_.rtt_ms);
        latest_bwe_.reset();
        last_twcc_time_ms_.reset();
        probe_controller_->Reset();
        ++probe_generation_;
    }
}

NetworkControlUpdate GoogCcNetworkController::OnProcessInterval(const ProcessInterval& msg)
{
    ExpireFeedback(msg.at_time_ms);
    // No synthetic feedback or bitrate growth on timer ticks.
    return GetNetworkState(msg.at_time_ms);
}

NetworkControlUpdate GoogCcNetworkController::OnRoundTripTimeUpdate(const RoundTripTimeUpdate& msg)
{
    if (!network_available_) return EmptyUpdate();
    ExpireFeedback(msg.at_time_ms);
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

    ExpireFeedback(msg.feedback.feedback_time_ms);

    const BweResult bwe = delay_bwe_.OnTransportFeedback(msg.feedback);
    latest_bwe_ = bwe;
    probe_controller_->OnFeedback(msg.feedback, delay_bwe_.GroupObservation(), bwe);

    latest_feedback_.now_ms = msg.feedback.feedback_time_ms;
    latest_feedback_.receive_bitrate_bps = bwe.incoming_bitrate_bps;
    latest_feedback_.loss_rate = msg.feedback.LossRate();
    latest_feedback_.delay_gradient_ms = bwe.delay_trend_ms;
    has_feedback_ = true;
    last_feedback_time_ms_ = last_twcc_time_ms_ = msg.feedback.feedback_time_ms;
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
    ExpireFeedback(feedback.now_ms);
    latest_feedback_ = feedback;
    has_feedback_ = true;
    last_feedback_time_ms_ = feedback.now_ms;
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

ProbeDecision GoogCcNetworkController::GetProbeDecision(const ProbeRequest& request)
{
    if (!network_available_) return {true, std::nullopt};
    auto decision = probe_controller_->OnProcessInterval({request, bitrate_,
        latest_update_.has_target_rate ? latest_update_.target_rate.target_bitrate_bps : 0,
        latest_feedback_.rtt_ms, latest_bwe_});
    if (decision.plan) decision.plan->generation = probe_generation_;
    return decision;
}

void GoogCcNetworkController::OnProbeEvent(const ProbeEvent& event)
{
    // 旧路径/超时前的执行事件不能进入已重置的算法实例。
    if (event.plan.generation == probe_generation_) probe_controller_->OnProbeEvent(event);
}

}
