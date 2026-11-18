#include <gtest/gtest.h>
#include "GoogCcNetworkController.h"
#include "ProbeController.h"
#include "SendSideController.h"

#include <algorithm>
#include <limits>

namespace
{
using namespace media;
ProbeContext Context(uint64_t now = 1000)
{
    ProbeContext context;
    context.request = {now, 1, 0, 400000};
    context.constraints = {100000, 200000, 2000000};
    context.target_bitrate_bps = 200000;
    context.rtt_ms = 80;
    return context;
}
ProbePlan Start(ProbeController& controller)
{
    auto decision = controller.OnProcessInterval(Context());
    EXPECT_TRUE(decision.handled);
    EXPECT_TRUE(decision.plan);
    auto plan = decision.plan.value_or(ProbePlan{});
    plan.cluster_id = 7;
    plan.generation = 1;
    controller.OnProbeEvent({ProbeEventType::Started, plan, 1, 1000, 0});
    return plan;
}
TransportFeedback Receipts(int cluster = 7)
{
    TransportFeedback feedback;
    feedback.feedback_time_ms = 1550;
    for (int i = 0; i < 62; ++i)
    {
        PacketFeedback packet;
        packet.sent_packet.transport_sequence = i;
        packet.sent_packet.send_time_ms = 1000 + i * 8;
        packet.sent_packet.size_bytes = 264;
        packet.sent_packet.is_probe = true;
        packet.sent_packet.probe_cluster_id = cluster;
        packet.received = true;
        packet.receive_time_ms = 50000 + i * 8; // A different clock offset is harmless.
        feedback.packet_feedbacks.push_back(packet);
    }
    return feedback;
}
void Finish(ProbeController& controller, const ProbePlan& plan)
{
    controller.OnProbeEvent({ProbeEventType::Finished, plan, 1, 1500, 62 * 300});
}

TEST(ProbeController, PlansExtraPaddingWithinTotalCapacityAndWaitsForCluster)
{
    ProbeController controller;
    auto context = Context();
    context.request.active_bitrate_bps = 100000;
    auto decision = controller.OnProcessInterval(context);
    ASSERT_TRUE(decision.plan);
    EXPECT_EQ(decision.plan->bitrate_bps, 200000U);
    EXPECT_EQ(decision.plan->pacing_bitrate_bps, 300000U);
    EXPECT_EQ(decision.plan->duration_ms, 500U);
    EXPECT_EQ(decision.plan->max_bytes, 12500U);
    Start(controller);
    EXPECT_FALSE(controller.OnProcessInterval(Context(1200)).plan);
}

TEST(ProbeController, RecoveryThresholdDrivesGoalAndSingleRoundGrowthRemainsBounded)
{
    ProbeController controller;
    auto context = Context();
    context.request.active_bitrate_bps = 1000000;
    context.request.candidate_bitrate_bps = 300000;
    context.request.required_budget_bps = 1600000;
    context.constraints = {100000, 1200000, 3000000};
    context.target_bitrate_bps = 1200000;
    auto decision = controller.OnProcessInterval(context);
    ASSERT_TRUE(decision.plan);
    EXPECT_EQ(decision.plan->pacing_bitrate_bps, 1800000U); // Capped at 1.5 times current target.
    EXPECT_EQ(decision.plan->bitrate_bps, 800000U); // Subtract active media, not the current estimate.
    context.target_bitrate_bps = 2000000;
    decision = controller.OnProcessInterval(context);
    ASSERT_TRUE(decision.plan);
    EXPECT_EQ(decision.plan->pacing_bitrate_bps, 2400000U); // 1.5 times the recovery threshold.
    EXPECT_EQ(decision.plan->bitrate_bps, 1400000U);
    context.request.required_budget_bps = 3000001;
    decision = controller.OnProcessInterval(context);
    EXPECT_FALSE(decision.plan);
    EXPECT_EQ(decision.reason, ProbeDecisionReason::RecoveryExceedsMaximum);
    context.request.required_budget_bps = std::numeric_limits<uint64_t>::max();
    EXPECT_EQ(controller.OnProcessInterval(context).reason, ProbeDecisionReason::RecoveryExceedsMaximum);
}

TEST(ProbeController, FreshCongestionBlocksStartAndFreshRecoveryAllowsIt)
{
    ProbeController controller;
    auto context = Context();
    context.bandwidth_estimate = BweResult{};
    context.bandwidth_estimate_time_ms = 900;
    context.bandwidth_estimate->usage = BandwidthUsage::kOverUsing;
    auto decision = controller.OnProcessInterval(context);
    EXPECT_FALSE(decision.plan);
    EXPECT_EQ(decision.reason, ProbeDecisionReason::Congested);
    EXPECT_STREQ(ToString(decision.reason), "Congested");

    context.bandwidth_estimate->usage = BandwidthUsage::kNormal;
    decision = controller.OnProcessInterval(context);
    EXPECT_TRUE(decision.plan);
    EXPECT_EQ(decision.reason, ProbeDecisionReason::Planned);
    for (const double loss : {0.06, std::numeric_limits<double>::quiet_NaN()})
    {
        context.bandwidth_estimate->loss_rate = loss;
        decision = controller.OnProcessInterval(context);
        EXPECT_FALSE(decision.plan);
        EXPECT_EQ(decision.reason, ProbeDecisionReason::Congested);
    }
}

TEST(ProbeController, SilentPausedVideoIsNotBlockedForeverByOldCongestion)
{
    ProbeController controller;
    auto context = Context(2000);
    context.bandwidth_estimate = BweResult{};
    context.bandwidth_estimate->usage = BandwidthUsage::kOverUsing;
    context.bandwidth_estimate_time_ms = 1000;
    EXPECT_EQ(controller.OnProcessInterval(context).reason, ProbeDecisionReason::Congested);
    context.request.now_ms = 2001;
    EXPECT_TRUE(controller.OnProcessInterval(context).plan);
    // Without the source feedback time, freshness cannot be asserted.
    context.bandwidth_estimate_time_ms.reset();
    EXPECT_TRUE(controller.OnProcessInterval(context).plan);
}

TEST(ProbeController, DecisionExplainsDemandCapacityAndStateGates)
{
    ProbeController controller;
    auto context = Context();
    context.request.candidate_bitrate_bps = 0;
    EXPECT_EQ(controller.OnProcessInterval(context).reason, ProbeDecisionReason::NoDemand);
    context = Context();
    context.request.active_bitrate_bps = context.constraints.max_bitrate_bps;
    EXPECT_EQ(controller.OnProcessInterval(context).reason, ProbeDecisionReason::NoHeadroom);
    context.request.active_bitrate_bps = 350000; // Below max, but above this round's ceiling.
    EXPECT_EQ(controller.OnProcessInterval(context).reason, ProbeDecisionReason::NoHeadroom);
    context = Context();
    context.request.candidate_bitrate_bps = 100;
    EXPECT_EQ(controller.OnProcessInterval(context).reason, ProbeDecisionReason::TooSmall);
    auto plan = Start(controller);
    EXPECT_EQ(controller.OnProcessInterval(Context(1100)).reason, ProbeDecisionReason::InProgress);
    controller.OnProbeEvent({ProbeEventType::Aborted, plan, 1, 1200, 0});
    EXPECT_EQ(controller.OnProcessInterval(Context(1201)).reason, ProbeDecisionReason::CoolingDown);
}

TEST(ProbeController, GrowingQueueStopsExecutionBeforeFinishedAfterEnoughObservation)
{
    ProbeController controller;
    const auto plan = Start(controller);
    auto feedback = Receipts();
    feedback.feedback_time_ms = 1200;
    feedback.packet_feedbacks.resize(13); // Only 96 ms: too short to judge queue growth.
    for (size_t i = 0; i < feedback.packet_feedbacks.size(); ++i)
        feedback.packet_feedbacks[i].receive_time_ms = 50000 + i * 12;
    controller.OnFeedback(feedback, {}, {});
    EXPECT_FALSE(controller.TakeResult());
    auto packet = feedback.packet_feedbacks.back();
    ++packet.sent_packet.transport_sequence;
    packet.sent_packet.send_time_ms += 8;
    packet.receive_time_ms += 12;
    feedback.packet_feedbacks.push_back(packet);
    controller.OnFeedback(feedback, {}, {});
    const auto result = controller.TakeResult();
    ASSERT_TRUE(result);
    EXPECT_EQ(result->outcome, ProbeOutcome::Congested);
    EXPECT_EQ(result->at_time_ms, 1200U);
    EXPECT_EQ(result->capacity_bps, 0U);
    Finish(controller, plan);
    EXPECT_FALSE(controller.TakeResult());
}

TEST(ProbeController, CurrentMediaFeedbackCanStopPaddingButOldMediaCannot)
{
    ProbeController controller;
    Start(controller);
    auto feedback = Receipts();
    feedback.feedback_time_ms = 1200;
    feedback.packet_feedbacks.resize(1);
    auto& packet = feedback.packet_feedbacks.front();
    packet.sent_packet.is_probe = false;
    packet.sent_packet.send_time_ms = 999;
    BweResult bwe;
    bwe.usage = BandwidthUsage::kOverUsing;
    controller.OnFeedback(feedback, {}, bwe);
    EXPECT_FALSE(controller.TakeResult());
    packet.sent_packet.send_time_ms = 1100;
    controller.OnFeedback(feedback, {}, bwe);
    const auto result = controller.TakeResult();
    ASSERT_TRUE(result);
    EXPECT_EQ(result->outcome, ProbeOutcome::Congested);
}

TEST(ProbeController, RawTwccLossStopsExecutionEvenIfSmoothedEstimateHasNoLoss)
{
    ProbeController controller;
    Start(controller);
    auto feedback = Receipts();
    feedback.feedback_time_ms = 1200;
    feedback.packet_feedbacks.resize(10);
    feedback.packet_feedbacks[0].received = false;
    feedback.packet_feedbacks[1].received = false;
    controller.OnFeedback(feedback, {}, {});
    const auto result = controller.TakeResult();
    ASSERT_TRUE(result);
    EXPECT_EQ(result->outcome, ProbeOutcome::Congested);
}

TEST(ProbeController, ExecutionWithoutReceiptsTimesOutAndBacksOff)
{
    ProbeController controller;
    auto plan = Start(controller);
    Finish(controller, plan);
    EXPECT_FALSE(controller.TakeResult());
    controller.OnTime(2499);
    EXPECT_FALSE(controller.TakeResult());
    controller.OnTime(2500);
    auto result = controller.TakeResult();
    ASSERT_TRUE(result);
    EXPECT_EQ(result->outcome, ProbeOutcome::TimedOut);
    EXPECT_EQ(result->capacity_bps, 0U);
    EXPECT_FALSE(controller.OnProcessInterval(Context(8499)).plan);
    EXPECT_TRUE(controller.OnProcessInterval(Context(8500)).plan);
}

TEST(ProbeController, MeasuresActualRateAndRequiresBothExecutionAndFeedback)
{
    ProbeController controller;
    auto plan = Start(controller);
    controller.OnFeedback(Receipts(), {}, {});
    EXPECT_FALSE(controller.TakeResult());
    Finish(controller, plan);
    auto result = controller.TakeResult();
    ASSERT_TRUE(result);
    EXPECT_EQ(result->outcome, ProbeOutcome::Succeeded);
    EXPECT_EQ(result->capacity_bps, 237600U); // 264 bytes / 8 ms, with 10% headroom.
    EXPECT_FALSE(controller.OnProcessInterval(Context(4499)).plan);
    EXPECT_TRUE(controller.OnProcessInterval(Context(4500)).plan);
}

TEST(ProbeController, ReorderedSplitFeedbackAndDuplicatesAreBoundedAndDeduplicated)
{
    ProbeController controller;
    auto plan = Start(controller);
    Finish(controller, plan);
    auto feedback = Receipts();
    auto second = feedback;
    feedback.packet_feedbacks.resize(31);
    second.packet_feedbacks.erase(second.packet_feedbacks.begin(), second.packet_feedbacks.begin() + 31);
    std::reverse(second.packet_feedbacks.begin(), second.packet_feedbacks.end());
    controller.OnFeedback(second, {}, {});
    for (int i = 0; i < 4; ++i) controller.OnFeedback(second, {}, {});
    EXPECT_FALSE(controller.TakeResult());
    controller.OnFeedback(feedback, {}, {});
    auto result = controller.TakeResult();
    ASSERT_TRUE(result);
    EXPECT_EQ(result->capacity_bps, 237600U);
    controller.OnFeedback(feedback, {}, {});
    EXPECT_FALSE(controller.TakeResult());
}

TEST(ProbeController, OtherClustersAndOldFeedbackCannotCompleteCurrentCluster)
{
    ProbeController controller;
    auto plan = Start(controller);
    Finish(controller, plan);
    controller.OnFeedback(Receipts(6), {}, {});
    EXPECT_FALSE(controller.TakeResult());
    controller.Reset();
    controller.OnFeedback(Receipts(), {}, {});
    EXPECT_FALSE(controller.TakeResult());
    EXPECT_TRUE(controller.OnProcessInterval(Context(1600)).plan);
}

TEST(ProbeController, LossOrGrowingQueueRejectsCapacity)
{
    for (int scenario = 0; scenario < 3; ++scenario)
    {
        ProbeController controller;
        auto plan = Start(controller);
        Finish(controller, plan);
        auto feedback = Receipts();
        BweResult bwe;
        if (scenario == 0)
            for (int i = 0; i < 6; ++i) feedback.packet_feedbacks[i].received = false;
        if (scenario == 1)
            for (size_t i = 0; i < feedback.packet_feedbacks.size(); ++i)
                feedback.packet_feedbacks[i].receive_time_ms = 50000 + i * 12;
        if (scenario == 2) bwe.usage = BandwidthUsage::kOverUsing;
        controller.OnFeedback(feedback, {}, bwe);
        auto result = controller.TakeResult();
        ASSERT_TRUE(result) << scenario;
        EXPECT_EQ(result->outcome, ProbeOutcome::Congested) << scenario;
        EXPECT_EQ(result->capacity_bps, 0U);
    }
}

TEST(ProbeController, CompressedArrivalsDoNotInventExtraCapacity)
{
    ProbeController controller;
    auto plan = Start(controller);
    Finish(controller, plan);
    auto feedback = Receipts();
    for (size_t i = 0; i < feedback.packet_feedbacks.size(); ++i)
        feedback.packet_feedbacks[i].receive_time_ms = 50000 + i;
    controller.OnFeedback(feedback, {}, {});
    auto result = controller.TakeResult();
    ASSERT_TRUE(result);
    EXPECT_EQ(result->capacity_bps, 237600U);
}

TEST(ProbeController, InsufficientActualSendRateDoesNotValidateScheduledRate)
{
    ProbeController controller;
    auto plan = Start(controller);
    Finish(controller, plan);
    auto feedback = Receipts();
    for (auto& packet : feedback.packet_feedbacks) packet.sent_packet.size_bytes = 100;
    controller.OnFeedback(feedback, {}, {});
    auto result = controller.TakeResult();
    ASSERT_TRUE(result);
    EXPECT_EQ(result->outcome, ProbeOutcome::Inconclusive);
    EXPECT_EQ(result->capacity_bps, 0U);
}

TEST(ProbeController, CountsOnlyActuallyReceivedActiveMediaAlongsidePadding)
{
    for (const bool include_media : {false, true})
    {
        ProbeController controller;
        auto context = Context();
        context.request.active_bitrate_bps = 100000;
        auto plan = *controller.OnProcessInterval(context).plan;
        plan.cluster_id = 7;
        controller.OnProbeEvent({ProbeEventType::Started, plan, 1, 1000, 0});
        auto feedback = Receipts();
        feedback.packet_feedbacks.resize(41);
        for (size_t i = 0; i < 41; ++i)
        {
            auto& packet = feedback.packet_feedbacks[i];
            packet.sent_packet.send_time_ms = 1000 + i * 12;
            packet.receive_time_ms = 50000 + i * 12;
        }
        if (include_media)
        {
            for (int i = 0; i < 40; ++i)
            {
                auto packet = feedback.packet_feedbacks[i];
                packet.sent_packet.transport_sequence = 1000 + i;
                packet.sent_packet.send_time_ms += 6;
                packet.receive_time_ms += 6;
                packet.sent_packet.size_bytes = 100;
                packet.sent_packet.is_probe = false;
                feedback.packet_feedbacks.push_back(packet);
            }
        }
        controller.OnProbeEvent({ProbeEventType::Finished, plan, 1, 1500, 41 * 300});
        controller.OnFeedback(feedback, {}, {});
        auto result = controller.TakeResult();
        ASSERT_TRUE(result);
        EXPECT_EQ(result->outcome, ProbeOutcome::Succeeded);
        EXPECT_EQ(result->capacity_bps, include_media ? 218400U : 158400U);
    }
}

TEST(ProbeController, RttExtendsFeedbackDeadlineAndAbortRejectsLateReceipts)
{
    ProbeController controller;
    auto context = Context();
    context.rtt_ms = 1000;
    auto decision = controller.OnProcessInterval(context);
    ASSERT_TRUE(decision.plan);
    auto plan = *decision.plan;
    plan.cluster_id = 7;
    controller.OnProbeEvent({ProbeEventType::Started, plan, 1, 1000, 0});
    Finish(controller, plan);
    controller.OnTime(3999);
    EXPECT_FALSE(controller.TakeResult());
    controller.OnProbeEvent({ProbeEventType::Aborted, plan, 1, 4000, 0});
    ASSERT_EQ(controller.TakeResult()->outcome, ProbeOutcome::Aborted);
    controller.OnFeedback(Receipts(), {}, {});
    EXPECT_FALSE(controller.TakeResult());
}

TEST(ProbeLoop, SuccessfulFeedbackRaisesBudgetAndPersistsInRateController)
{
    NetworkControllerConfig config;
    config.bitrate = Context().constraints;
    GoogCcNetworkController controller(config);
    auto plan = *controller.GetProbeDecision(Context().request).plan;
    plan.cluster_id = 7;
    controller.OnProbeEvent({ProbeEventType::Started, plan, 1, 1000, 0});
    controller.OnProbeEvent({ProbeEventType::Finished, plan, 1, 1500, 62 * 300});
    EXPECT_FALSE(controller.GetNetworkState(1500).has_target_rate);
    auto update = controller.OnTransportPacketsFeedback({Receipts()});
    ASSERT_TRUE(controller.LastProbeResult());
    EXPECT_EQ(controller.LastProbeResult()->outcome, ProbeOutcome::Succeeded);
    EXPECT_EQ(update.target_rate.target_bitrate_bps, 237600U);
    EXPECT_EQ(update.target_rate.estimate.bandwidth_bps, 237600U);
    EXPECT_GT(update.pacer_config.pacing_bitrate_bps, 237600U);
    auto next = Receipts();
    next.feedback_time_ms = 1650;
    for (auto& packet : next.packet_feedbacks)
    {
        packet.sent_packet.transport_sequence += 100;
        packet.sent_packet.send_time_ms += 100;
        packet.receive_time_ms += 100;
        packet.sent_packet.is_probe = false;
    }
    update = controller.OnTransportPacketsFeedback({next});
    EXPECT_GE(update.target_rate.target_bitrate_bps, 237600U);
    controller.OnNetworkRouteChange({1700, config.bitrate});
    EXPECT_FALSE(controller.LastProbeResult());
    EXPECT_FALSE(controller.GetNetworkState(1700).has_target_rate);
}

TEST(ProbeLoop, AlreadyReceivedFeedbackCanCompleteOnFinishedEvent)
{
    NetworkControllerConfig config;
    config.bitrate = Context().constraints;
    GoogCcNetworkController controller(config);
    auto plan = *controller.GetProbeDecision(Context().request).plan;
    plan.cluster_id = 7;
    controller.OnProbeEvent({ProbeEventType::Started, plan, 1, 1000, 0});
    auto feedback = Receipts();
    feedback.feedback_time_ms = 1499;
    controller.OnTransportPacketsFeedback({feedback});
    EXPECT_FALSE(controller.LastProbeResult());
    const auto update = controller.OnProbeEvent({ProbeEventType::Finished, plan, 1, 1500, 62 * 300});
    EXPECT_EQ(update.target_rate.target_bitrate_bps, 237600U);
}

TEST(ProbeLoop, CongestedFeedbackCancelsPacingBeforeExecutionDeadline)
{
    NetworkControllerConfig config;
    config.bitrate = Context().constraints;
    SendSideController controller(config);
    auto decision = controller.GetProbeDecision(Context().request);
    auto plan = controller.StartProbe(*decision.plan, 1000, 1);
    ASSERT_TRUE(plan);
    for (int i = 0; i < 10; ++i)
    {
        PacketSendInfo sent;
        sent.transport_sequence = i;
        sent.send_time_ms = 1000 + i * 20;
        sent.size_bytes = 264;
        sent.is_probe = true;
        sent.probe_cluster_id = plan->cluster_id;
        controller.OnPacketSent(sent);
    }
    // TWCC run-length chunk with ten lost statuses and no receive deltas.
    const uint8_t feedback[24]{0x8f, 205, 0, 5, 0, 0, 0, 0, 0, 0, 0, 0,
                               0, 0, 0, 10, 0, 0, 0, 0, 0, 10, 0, 0};
    ASSERT_TRUE(controller.OnRtcpPacket(feedback, sizeof(feedback), 1250));
    ASSERT_TRUE(controller.LastProbeResult());
    EXPECT_EQ(controller.LastProbeResult()->outcome, ProbeOutcome::Congested);
    EXPECT_FALSE(controller.IsProbeActive(plan->cluster_id, 1250));
    EXPECT_LT(controller.GetPacingRate(1250), 300000U);
}

TEST(ProbeLoop, PacingOverrideExpiresAndOldClustersCannotReviveIt)
{
    NetworkControllerConfig config;
    config.bitrate = Context().constraints;
    SendSideController controller(config);
    auto plan = *controller.GetProbeDecision(Context().request).plan;
    auto started = controller.StartProbe(plan, 1000, 1);
    ASSERT_TRUE(started);
    EXPECT_EQ(controller.GetPacingRate(1100), 300000U);
    EXPECT_TRUE(controller.IsProbeActive(started->cluster_id, 1499));
    EXPECT_FALSE(controller.IsProbeActive(started->cluster_id, 1500));
    EXPECT_EQ(controller.GetPacingRate(1500), 200000U);
    EXPECT_FALSE(controller.StartProbe(plan, 1200, 1));
    controller.OnProbeEvent({ProbeEventType::Aborted, *started, 1, 1300, 0});
    EXPECT_EQ(controller.GetPacingRate(1300), 200000U);
    controller.OnNetworkRouteChanged();
    EXPECT_FALSE(controller.StartProbe(plan, 1400, 1));
    EXPECT_FALSE(controller.IsProbeActive(started->cluster_id, 1400));
    controller.SetNetworkAvailable(false);
    EXPECT_EQ(controller.GetPacingRate(1400), 0U);
}
}
