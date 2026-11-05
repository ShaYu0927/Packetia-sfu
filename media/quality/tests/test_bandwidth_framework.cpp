#include <gtest/gtest.h>

#include "GoogCcNetworkController.h"
#include "SendSideController.h"

#include <limits>

namespace
{
using namespace media;

struct AlgorithmCalls
{
    int group_feedbacks = 0;
    int group_resets = 0;
    int probe_feedbacks = 0;
    int probe_resets = 0;
    PacketGroupContext group_context;
    ProbeContext probe_context;
    PacketGroupObservation observation;
    ProbeDecision decision;
    std::vector<ProbeEvent> events;
    std::vector<int> feedback_probe_ids;
};

class TestGroups final : public IPacketGroupObserver
{
public:
    explicit TestGroups(std::shared_ptr<AlgorithmCalls> calls) : calls_(std::move(calls)) {}
    PacketGroupObservation OnFeedback(const TransportFeedback&, const PacketGroupContext& context) override
    {
        ++calls_->group_feedbacks;
        calls_->group_context = context;
        return calls_->observation;
    }
    void Reset() override { ++calls_->group_resets; }
private:
    std::shared_ptr<AlgorithmCalls> calls_;
};

class TestProbes final : public IProbeController
{
public:
    explicit TestProbes(std::shared_ptr<AlgorithmCalls> calls) : calls_(std::move(calls)) {}
    ProbeDecision OnProcessInterval(const ProbeContext& context) override
    {
        calls_->probe_context = context;
        return calls_->decision;
    }
    void OnFeedback(const TransportFeedback& feedback, const PacketGroupObservation& groups,
                    const BweResult&) override
    {
        ++calls_->probe_feedbacks;
        EXPECT_EQ(groups.completed_groups.size(), calls_->observation.completed_groups.size());
        for (const auto& packet : feedback.packet_feedbacks)
            if (packet.sent_packet.is_probe) calls_->feedback_probe_ids.push_back(packet.sent_packet.probe_cluster_id);
    }
    void OnProbeEvent(const ProbeEvent& event) override { calls_->events.push_back(event); }
    void Reset() override { ++calls_->probe_resets; }
private:
    std::shared_ptr<AlgorithmCalls> calls_;
};

NetworkControllerConfig Config(const std::shared_ptr<AlgorithmCalls>& calls)
{
    NetworkControllerConfig config;
    config.packet_group_observer_factory = [calls] { return std::make_unique<TestGroups>(calls); };
    config.probe_controller_factory = [calls] { return std::make_unique<TestProbes>(calls); };
    return config;
}

TransportFeedback Feedback(uint64_t at)
{
    TransportFeedback feedback;
    feedback.feedback_time_ms = at;
    for (int i = 0; i < 10; ++i)
    {
        PacketFeedback packet;
        packet.sent_packet.transport_sequence = i;
        packet.sent_packet.send_time_ms = at - 200 + i * 15;
        packet.sent_packet.size_bytes = 1200;
        packet.receive_time_ms = at - 150 + i * 15;
        packet.received = i != 9;
        packet.sent_packet.is_probe = true;
        packet.sent_packet.probe_cluster_id = 12;
        feedback.packet_feedbacks.push_back(packet);
    }
    return feedback;
}

TEST(BandwidthFramework, EmptyAlgorithmsPreserveExistingEstimationAndLegacyProbeSelection)
{
    GoogCcNetworkController defaults;
    auto calls = std::make_shared<AlgorithmCalls>();
    GoogCcNetworkController injected(Config(calls));
    for (uint64_t at : {1000, 3200, 3500})
    {
        const auto feedback = Feedback(at);
        const auto a = defaults.OnTransportPacketsFeedback({feedback});
        const auto b = injected.OnTransportPacketsFeedback({feedback});
        EXPECT_EQ(a.target_rate.target_bitrate_bps, b.target_rate.target_bitrate_bps);
        EXPECT_EQ(a.target_rate.estimate.bandwidth_bps, b.target_rate.estimate.bandwidth_bps);
        EXPECT_EQ(a.target_rate.estimate.recv_bitrate_bps, b.target_rate.estimate.recv_bitrate_bps);
    }
    EXPECT_EQ(calls->group_feedbacks, 3);
    EXPECT_EQ(calls->probe_feedbacks, 3);
    EXPECT_FALSE(defaults.GetProbeDecision({3500, 1, 32000, 800000}).handled);
    EXPECT_FALSE(injected.GetProbeDecision({3500, 1, 32000, 800000}).handled);
}

TEST(BandwidthFramework, GroupOutputReachesRateControlAndProbeFeedbackWithClockContext)
{
    auto calls = std::make_shared<AlgorithmCalls>();
    calls->observation.completed_groups.push_back({});
    calls->observation.receive_bitrate_bps = 350000;
    calls->observation.usage = BandwidthUsage::kOverUsing;
    calls->observation.delay_trend_ms = 73.0;
    calls->observation.loss_rate = 0.0;
    GoogCcNetworkController controller(Config(calls));
    controller.OnRoundTripTimeUpdate({900, 80});
    const auto update = controller.OnTransportPacketsFeedback({Feedback(1000)});
    EXPECT_EQ(calls->group_context.now_ms, 1000U);
    EXPECT_EQ(calls->group_context.rtt_ms, 80U);
    EXPECT_EQ(calls->group_context.target_bitrate_bps, BitrateConstraints{}.start_bitrate_bps);
    EXPECT_EQ(update.target_rate.estimate.recv_bitrate_bps, 350000U);
    EXPECT_LT(update.target_rate.target_bitrate_bps, 350000U);
    EXPECT_EQ(calls->probe_feedbacks, 1);
    EXPECT_EQ(calls->feedback_probe_ids.size(), 10U);
    controller.GetProbeDecision({1100, 1, 32000, 500000});
    EXPECT_EQ(calls->probe_context.request.active_bitrate_bps, 32000U);
    ASSERT_TRUE(calls->probe_context.bandwidth_estimate);
    EXPECT_DOUBLE_EQ(calls->probe_context.bandwidth_estimate->delay_trend_ms, 73.0);
}

TEST(BandwidthFramework, MissingAndNonfiniteGroupResultsUsePerPacketFallback)
{
    auto calls = std::make_shared<AlgorithmCalls>();
    calls->observation.loss_rate = std::numeric_limits<double>::quiet_NaN();
    calls->observation.delay_trend_ms = std::numeric_limits<double>::infinity();
    GoogCcNetworkController injected(Config(calls)), baseline;
    const auto a = injected.OnTransportPacketsFeedback({Feedback(1000)});
    const auto b = baseline.OnTransportPacketsFeedback({Feedback(1000)});
    EXPECT_EQ(a.target_rate.target_bitrate_bps, b.target_rate.target_bitrate_bps);
    EXPECT_EQ(a.target_rate.estimate.recv_bitrate_bps, b.target_rate.estimate.recv_bitrate_bps);
}

TEST(BandwidthFramework, TimeoutResetsAlgorithmsAndRejectsOldProbeEventsWithoutChangingPath)
{
    auto calls = std::make_shared<AlgorithmCalls>();
    calls->decision = {true, ProbePlan{100000, 100, 2000}};
    SendSideController controller(Config(calls));
    WeakNetFeedback rr;
    rr.now_ms = 1000;
    controller.OnReceiverFeedback(rr);
    const auto budget = controller.GetBandwidthBudget();
    auto decision = controller.GetProbeDecision({1000, budget.path_epoch, 0, 500000});
    ASSERT_TRUE(decision.plan);
    const auto plan = controller.StartProbe(*decision.plan, 1000, budget.path_epoch);
    ASSERT_TRUE(plan);
    ASSERT_EQ(calls->events.size(), 1U);
    const auto resets = calls->group_resets;
    controller.OnProcessInterval(6000);
    EXPECT_GT(calls->group_resets, resets);
    EXPECT_EQ(calls->probe_resets, 1);
    EXPECT_EQ(controller.GetBandwidthBudget().path_epoch, budget.path_epoch);
    EXPECT_NE(controller.GetBandwidthBudget().probe_generation, plan->generation);
    controller.OnProbeEvent({ProbeEventType::Finished, *plan, budget.path_epoch, 6000, 1000});
    EXPECT_EQ(calls->events.size(), 1U);
    EXPECT_FALSE(controller.StartProbe(*plan, 6000, budget.path_epoch));
    decision = controller.GetProbeDecision({6001, budget.path_epoch, 0, 500000});
    ASSERT_TRUE(decision.plan);
    const auto next = controller.StartProbe(*decision.plan, 6001, budget.path_epoch);
    ASSERT_TRUE(next);
    EXPECT_GT(next->cluster_id, plan->cluster_id);
    controller.SetNetworkAvailable(false);
    EXPECT_FALSE(controller.GetProbeDecision({6002, budget.path_epoch, 0, 500000}).plan);
    controller.OnProbeEvent({ProbeEventType::Finished, *next, budget.path_epoch, 6002, 1000});
    EXPECT_EQ(calls->events.size(), 2U);
}
}
