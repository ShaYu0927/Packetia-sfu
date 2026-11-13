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

PacketFeedback GroupPacket(int64_t sequence, uint64_t send_ms, uint64_t receive_ms,
                           bool received = true, uint32_t bytes = 1200)
{
    PacketFeedback packet;
    packet.sent_packet.transport_sequence = sequence;
    packet.sent_packet.send_time_ms = send_ms;
    packet.sent_packet.size_bytes = bytes;
    packet.received = received;
    packet.receive_time_ms = receive_ms;
    return packet;
}

TEST(PacketGrouping, PreservesPendingGroupAcrossFeedbackAndIncludesFiveMillisecondBoundary)
{
    PacketGroupObserver observer;
    TransportFeedback feedback;
    feedback.feedback_time_ms = 1000;
    feedback.packet_feedbacks = {GroupPacket(2, 105, 190, true, 200),
                                GroupPacket(0, 100, 200, true, 100),
                                GroupPacket(1, 104, 999, false, 300)};
    EXPECT_TRUE(observer.OnFeedback(feedback, {}).completed_groups.empty());

    feedback.feedback_time_ms = 1010;
    feedback.packet_feedbacks = {GroupPacket(3, 106, 210)};
    const auto result = observer.OnFeedback(feedback, {});
    ASSERT_EQ(result.completed_groups.size(), 1U);
    const auto& group = result.completed_groups.front();
    EXPECT_EQ(group.id, 0U);
    EXPECT_EQ(group.feedback_time_ms, 1010U);
    EXPECT_EQ(group.first_send_time_ms, 100U);
    EXPECT_EQ(group.last_send_time_ms, 105U);
    ASSERT_TRUE(group.first_receive_time_ms);
    ASSERT_TRUE(group.last_receive_time_ms);
    EXPECT_EQ(*group.first_receive_time_ms, 190U);
    EXPECT_EQ(*group.last_receive_time_ms, 200U);
    EXPECT_EQ(group.received_packets, 2U);
    EXPECT_EQ(group.received_bytes, 300U);
    EXPECT_EQ(group.lost_packets, 1U);
    EXPECT_EQ(group.lost_bytes, 300U);
    EXPECT_FALSE(group.queue_delay_ms);
    EXPECT_FALSE(result.usage);
    EXPECT_FALSE(result.delay_trend_ms);
    EXPECT_FALSE(result.receive_bitrate_bps);
    EXPECT_FALSE(result.loss_rate);

    feedback.packet_feedbacks = {GroupPacket(4, 111, 220), GroupPacket(5, 112, 230)};
    const auto next = observer.OnFeedback(feedback, {});
    ASSERT_EQ(next.completed_groups.size(), 1U);
    EXPECT_EQ(next.completed_groups.front().id, 1U);
    EXPECT_EQ(next.completed_groups.front().first_send_time_ms, 106U);
    EXPECT_EQ(next.completed_groups.front().last_send_time_ms, 111U);
    EXPECT_EQ(next.completed_groups.front().received_packets, 2U);
}

TEST(PacketGrouping, DuplicateFeedbackAndLatePacketsDoNotModifyCompletedGroups)
{
    PacketGroupObserver observer;
    TransportFeedback feedback;
    feedback.packet_feedbacks = {GroupPacket(0, 100, 150), GroupPacket(1, 106, 156)};
    ASSERT_EQ(observer.OnFeedback(feedback, {}).completed_groups.size(), 1U);
    EXPECT_TRUE(observer.OnFeedback(feedback, {}).completed_groups.empty());

    feedback.packet_feedbacks = {GroupPacket(2, 102, 152), GroupPacket(3, 107, 157),
                                GroupPacket(4, 112, 162)};
    const auto result = observer.OnFeedback(feedback, {});
    ASSERT_EQ(result.completed_groups.size(), 1U);
    const auto& group = result.completed_groups.front();
    EXPECT_EQ(group.id, 1U);
    EXPECT_EQ(group.first_send_time_ms, 106U);
    EXPECT_EQ(group.last_send_time_ms, 107U);
    EXPECT_EQ(group.received_packets, 2U);
    EXPECT_EQ(group.received_bytes, 2400U);
}

TEST(PacketGrouping, LostOnlyGroupsHaveNoReceiveTimesAndResetClearsGroupingAndDeduplication)
{
    PacketGroupObserver observer;
    TransportFeedback feedback;
    feedback.packet_feedbacks = {GroupPacket(0, 0, 999, false), GroupPacket(1, 6, 0)};
    const auto result = observer.OnFeedback(feedback, {});
    ASSERT_EQ(result.completed_groups.size(), 1U);
    EXPECT_EQ(result.completed_groups.front().lost_packets, 1U);
    EXPECT_EQ(result.completed_groups.front().received_packets, 0U);
    EXPECT_FALSE(result.completed_groups.front().first_receive_time_ms);
    EXPECT_FALSE(result.completed_groups.front().last_receive_time_ms);

    observer.Reset();
    feedback.packet_feedbacks = {GroupPacket(0, 0, 0), GroupPacket(1, 6, 6)};
    const auto restarted = observer.OnFeedback(feedback, {});
    ASSERT_EQ(restarted.completed_groups.size(), 1U);
    const auto& group = restarted.completed_groups.front();
    EXPECT_EQ(group.id, 0U);
    EXPECT_EQ(group.first_send_time_ms, 0U);
    EXPECT_EQ(group.received_packets, 1U);
    EXPECT_EQ(group.lost_packets, 0U);
    ASSERT_TRUE(group.first_receive_time_ms);
    EXPECT_EQ(*group.first_receive_time_ms, 0U);
}

PacketGroupObservation ObservePacket(PacketGroupObserver& observer, int64_t sequence,
                                     uint64_t send_ms, uint64_t receive_ms)
{
    TransportFeedback feedback;
    feedback.feedback_time_ms = 1000000 + static_cast<uint64_t>(sequence) * 20;
    feedback.packet_feedbacks = {GroupPacket(sequence, send_ms, receive_ms)};
    return observer.OnFeedback(feedback, {});
}

TEST(GroupTrend, WaitsForWindowAndDetectsStableGrowingAndDrainingQueues)
{
    for (const int receive_interval : {20, 25, 15})
    {
        SCOPED_TRACE(receive_interval);
        PacketGroupObserver observer;
        PacketGroupObservation result;
        for (int i = 0; i < 80; ++i)
        {
            result = ObservePacket(observer, i, 1000 + i * 20, 10000 + i * receive_interval);
            if (i <= 20)
            {
                EXPECT_FALSE(result.usage);
                EXPECT_FALSE(result.delay_trend_ms);
                EXPECT_FALSE(result.delay_trend_slope);
            }
        }
        ASSERT_TRUE(result.usage);
        ASSERT_TRUE(result.delay_trend_ms);
        ASSERT_TRUE(result.delay_trend_slope);
        if (receive_interval == 20)
        {
            EXPECT_EQ(*result.usage, BandwidthUsage::kNormal);
            EXPECT_DOUBLE_EQ(*result.delay_trend_ms, 0);
            EXPECT_DOUBLE_EQ(*result.delay_trend_slope, 0);
        }
        else if (receive_interval == 25)
        {
            EXPECT_EQ(*result.usage, BandwidthUsage::kOverUsing);
            EXPECT_NEAR(*result.delay_trend_slope, 0.2, 0.001);
            EXPECT_GT(*result.delay_trend_ms, 0);
        }
        else
        {
            EXPECT_EQ(*result.usage, BandwidthUsage::kUnderUsing);
            EXPECT_NEAR(*result.delay_trend_slope, -1.0 / 3.0, 0.001);
            EXPECT_LT(*result.delay_trend_ms, 0);
        }
    }
}

TEST(GroupTrend, SmallTransientSpikeDoesNotTriggerOveruse)
{
    PacketGroupObserver observer;
    for (int i = 0; i < 100; ++i)
    {
        const auto result = ObservePacket(observer, i, 1000 + i * 20,
            10000 + i * 20 + (i == 50 ? 10 : 0));
        if (result.usage) EXPECT_EQ(*result.usage, BandwidthUsage::kNormal);
    }
}

TEST(GroupTrend, LostTailPacketsAndLostOnlyGroupsDoNotDistortDelayTiming)
{
    PacketGroupObserver observer;
    PacketGroupObservation result;
    for (int i = 0; i < 80; ++i)
    {
        TransportFeedback feedback;
        const bool received = i != 40;
        feedback.packet_feedbacks = {
            GroupPacket(i * 2, 1000 + i * 20, 10000 + i * 20, received),
            GroupPacket(i * 2 + 1, 1000 + i * 20 + (i % 2 ? 5 : 1), 0, false)};
        result = observer.OnFeedback(feedback, {});
        ASSERT_EQ(result.completed_groups.size(), i == 0 ? 0U : 1U);
        if (i == 1)
        {
            EXPECT_EQ(result.completed_groups.front().last_send_time_ms, 1001U);
            EXPECT_EQ(result.completed_groups.front().last_received_send_time_ms, 1000U);
        }
        if (result.usage) EXPECT_EQ(*result.usage, BandwidthUsage::kNormal);
    }
    ASSERT_TRUE(result.delay_trend_slope);
    ASSERT_TRUE(result.delay_trend_ms);
    EXPECT_DOUBLE_EQ(*result.delay_trend_slope, 0);
    EXPECT_DOUBLE_EQ(*result.delay_trend_ms, 0);
}

TEST(GroupTrend, BackwardsArrivalIsSkippedWithoutMovingReference)
{
    PacketGroupObserver observer;
    for (int i = 0; i < 80; ++i)
        ObservePacket(observer, i, 1000 + i * 20, 10000 + i * 20);
    ObservePacket(observer, 80, 2600, 11400);
    const auto skipped = ObservePacket(observer, 81, 2620, 11620);
    EXPECT_FALSE(skipped.usage);
    const auto resumed = ObservePacket(observer, 82, 2640, 11640);
    ASSERT_TRUE(resumed.usage);
    EXPECT_EQ(*resumed.usage, BandwidthUsage::kNormal);
    EXPECT_EQ(resumed.delay_trend_ms, 0);
    EXPECT_EQ(resumed.delay_trend_slope, 0);
}

TEST(GroupTrend, IdleGapAndResetDiscardOldTrendAndRequireNewWarmup)
{
    PacketGroupObserver observer;
    for (int i = 0; i < 80; ++i)
        ObservePacket(observer, i, 1000 + i * 20, 10000 + i * 25);

    TransportFeedback feedback;
    feedback.packet_feedbacks = {GroupPacket(80, 100000, 200000), GroupPacket(81, 100020, 200020)};
    const auto gap = observer.OnFeedback(feedback, {});
    EXPECT_FALSE(gap.usage);
    EXPECT_FALSE(gap.delay_trend_ms);
    EXPECT_FALSE(gap.delay_trend_slope);
    for (int i = 82; i <= 101; ++i)
    {
        const auto result = ObservePacket(observer, i, 100000 + (i - 80) * 20, 200000 + (i - 80) * 20);
        if (i < 101) EXPECT_FALSE(result.usage);
        else
        {
            ASSERT_TRUE(result.usage);
            EXPECT_EQ(*result.usage, BandwidthUsage::kNormal);
            EXPECT_EQ(result.delay_trend_ms, 0);
        }
    }

    observer.Reset();
    for (int i = 0; i < 22; ++i)
    {
        const auto result = ObservePacket(observer, i, 1000 + i * 20, 10000 + i * 20);
        if (i <= 20) EXPECT_FALSE(result.usage);
        else
        {
            ASSERT_TRUE(result.usage);
            EXPECT_EQ(*result.usage, BandwidthUsage::kNormal);
            EXPECT_EQ(result.delay_trend_ms, 0);
        }
    }
}

TEST(GroupTrend, IndependentLargeClockOffsetsPreserveTrend)
{
    PacketGroupObserver baseline, shifted;
    const uint64_t large_origin = std::numeric_limits<uint64_t>::max() - 10000;
    for (int i = 0; i < 80; ++i)
    {
        const auto a = ObservePacket(baseline, i, 1000 + i * 20, 10000 + i * 25);
        const auto b = ObservePacket(shifted, i, large_origin + i * 20, large_origin + i * 25);
        EXPECT_EQ(a.usage, b.usage);
        EXPECT_EQ(a.delay_trend_ms, b.delay_trend_ms);
        EXPECT_EQ(a.delay_trend_slope, b.delay_trend_slope);
    }
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
