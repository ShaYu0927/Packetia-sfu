#include <gtest/gtest.h>

#include "DownlinkBandwidthAllocator.h"
#include "GoogCcNetworkController.h"
#include "NetworkStatsCollector.h"
#include "ReceiveQualityMonitor.h"
#include "estimator/EwmaFilter.h"

#include <cmath>
#include <limits>
#include <stdexcept>

namespace
{
using namespace media;

TEST(EwmaSampling, FixedAlphaRejectsInvalidDuplicateAndBackwardsSamplesAndCanReset)
{
    EXPECT_THROW((void)EwmaFilter(-0.1), std::invalid_argument);
    EXPECT_THROW((void)EwmaFilter(1.1), std::invalid_argument);
    EXPECT_THROW((void)EwmaFilter(std::numeric_limits<double>::quiet_NaN()), std::invalid_argument);
    EwmaFilter filter(0.75);
    EXPECT_FALSE(filter.GetValue(0));
    filter.AddSample(100, 0);
    filter.AddSample(0, 1);
    filter.AddSample(1000, 1);
    filter.AddSample(1000, 0);
    filter.AddSample(std::numeric_limits<double>::infinity(), 2);
    EXPECT_DOUBLE_EQ(*filter.GetValue(2), 75);
    EXPECT_EQ(filter.LastSampleTimeMs(), 1U);
    EXPECT_FALSE(filter.GetValue(0));
    filter.Reset();
    EXPECT_FALSE(filter.LastSampleTimeMs());
    EXPECT_FALSE(filter.GetValue(2));
    filter.AddSample(200, 0);
    EXPECT_DOUBLE_EQ(*filter.GetValue(0), 200);
}

TEST(EwmaSampling, TimeConstantDependsOnElapsedTimeRatherThanFeedbackCount)
{
    EwmaFilter slow(0.75), fast(0.75);
    ASSERT_TRUE(slow.SetTimeConstantMs(1000));
    ASSERT_TRUE(fast.SetTimeConstantMs(1000));
    EXPECT_FALSE(slow.SetTimeConstantMs(0));
    slow.AddSample(0, 0); fast.AddSample(0, 0);
    slow.AddSample(100, 1000);
    for (uint64_t at = 100; at <= 1000; at += 100) fast.AddSample(100, at);
    EXPECT_NEAR(*slow.GetValue(1000), 100 * (1 - std::exp(-1)), 1e-10);
    EXPECT_NEAR(*fast.GetValue(1000), *slow.GetValue(1000), 1e-10);
    slow.Reset();
    slow.AddSample(0, 1000); slow.AddSample(100, 2000);
    EXPECT_NEAR(*slow.GetValue(2000), 100 * (1 - std::exp(-1)), 1e-10);
}

WeakNetFeedback Metrics(uint64_t at)
{
    WeakNetFeedback feedback;
    feedback.now_ms = at;
    feedback.send_bitrate_bps = 1000000;
    feedback.receive_bitrate_bps = 800000;
    feedback.rtt_ms = 100;
    feedback.jitter_ms = 10;
    feedback.loss_rate = 0.0;
    return feedback;
}

TEST(MetricSampling, SmoothsAllMetricsAndExpiresEachMetricIndependently)
{
    NetworkStatsCollector stats;
    EXPECT_FALSE(stats.GetSnapshot(0).rtt_ms);
    stats.OnFeedback(Metrics(1000));
    auto next = Metrics(2000);
    next.send_bitrate_bps = 2000000;
    next.receive_bitrate_bps = 1600000;
    next.rtt_ms = 300;
    next.jitter_ms = 100;
    next.loss_rate = 0.4;
    stats.OnFeedback(next);
    const auto smooth = stats.GetSnapshot(2000);
    const double weight = 1 - std::exp(-1);
    ASSERT_TRUE(smooth.send_bitrate_bps && smooth.recv_bitrate_bps && smooth.rtt_ms &&
        smooth.jitter_ms && smooth.loss_rate);
    EXPECT_NEAR(*smooth.send_bitrate_bps, 1000000 + 1000000 * weight, 1e-6);
    EXPECT_NEAR(*smooth.recv_bitrate_bps, 800000 + 800000 * weight, 1e-6);
    EXPECT_NEAR(*smooth.rtt_ms, 100 + 200 * weight, 1e-6);
    EXPECT_NEAR(*smooth.jitter_ms, 10 + 90 * weight, 1e-6);
    EXPECT_NEAR(*smooth.loss_rate, 0.4 * weight, 1e-10);
    stats.OnRttSample(500, 5001);
    const auto expired = stats.GetSnapshot(5001);
    EXPECT_FALSE(expired.send_bitrate_bps);
    EXPECT_FALSE(expired.recv_bitrate_bps);
    EXPECT_FALSE(expired.jitter_ms);
    EXPECT_FALSE(expired.loss_rate);
    ASSERT_TRUE(expired.rtt_ms);
    EXPECT_DOUBLE_EQ(*expired.rtt_ms, 500); // New measurement after a gap starts fresh.
    stats.Reset();
    EXPECT_FALSE(stats.GetSnapshot(5001).rtt_ms);
}

TEST(MetricSampling, MissingSamplesDoNotRefreshTheirAgeAndInvalidLossIsIgnored)
{
    NetworkStatsCollector stats;
    auto feedback = Metrics(1000);
    feedback.rtt_ms = 0;
    stats.OnFeedback(feedback);
    EXPECT_FALSE(stats.GetSnapshot(1000).rtt_ms);
    stats.OnTransportFeedback(0, std::numeric_limits<double>::quiet_NaN(), 2000);
    stats.OnTransportFeedback(0, 2.0, 2500);
    stats.OnRttSample(0, 4001);
    const auto expired = stats.GetSnapshot(4001);
    EXPECT_FALSE(expired.recv_bitrate_bps);
    EXPECT_FALSE(expired.loss_rate);
    EXPECT_FALSE(expired.rtt_ms);
    stats.OnTransportFeedback(400000, 0.1, 4002);
    EXPECT_DOUBLE_EQ(*stats.GetSnapshot(4002).recv_bitrate_bps, 400000);
    EXPECT_DOUBLE_EQ(*stats.GetSnapshot(4002).loss_rate, 0.1);
}

TEST(MetricSampling, RawCongestionStillImmediatelyTriggersQualityAndKeyframeRecovery)
{
    WeakNetController controller;
    controller.OnFeedback(Metrics(1000));
    auto bad = Metrics(1100);
    bad.loss_rate = 0.3; bad.rtt_ms = 600;
    const auto update = controller.OnFeedback(bad);
    const auto& estimate = update.target_rate.estimate;
    EXPECT_EQ(estimate.quality, NetworkQualityLevel::Bad);
    EXPECT_TRUE(update.request_key_frame);
    EXPECT_DOUBLE_EQ(estimate.loss_rate, 0.3);
    EXPECT_EQ(estimate.rtt_ms, 600U);
    ASSERT_TRUE(estimate.smoothed.loss_rate && estimate.smoothed.rtt_ms);
    EXPECT_LT(*estimate.smoothed.loss_rate, 0.2);
    EXPECT_LT(*estimate.smoothed.rtt_ms, 500);
    EXPECT_EQ(controller.GetSnapshot().smoothed.loss_rate, estimate.smoothed.loss_rate);
    EXPECT_FALSE(controller.GetSnapshot(4101).smoothed.loss_rate);
}

TransportFeedback Packets(uint64_t at)
{
    TransportFeedback feedback;
    feedback.feedback_time_ms = at;
    for (unsigned i = 0; i < 10; ++i)
    {
        PacketFeedback packet;
        packet.sent_packet.transport_sequence = at + i;
        packet.sent_packet.send_time_ms = at - 200 + i * 12;
        packet.sent_packet.size_bytes = 1250;
        packet.received = true;
        packet.receive_time_ms = at - 150 + i * 12;
        feedback.packet_feedbacks.push_back(packet);
    }
    return feedback;
}

TEST(MetricSampling, TransportRttEventsAndTwccDoNotResampleUnrelatedMetrics)
{
    GoogCcNetworkController controller;
    controller.OnReceiverFeedback(Metrics(1000));
    const auto twcc = controller.OnTransportPacketsFeedback({Packets(2000)});
    ASSERT_TRUE(twcc.target_rate.estimate.smoothed.recv_bitrate_bps);
    controller.OnRoundTripTimeUpdate({4500, 500});
    const auto snapshot = controller.GetNetworkState(5101).target_rate.estimate;
    EXPECT_EQ(snapshot.rtt_ms, 500U);
    ASSERT_TRUE(snapshot.smoothed.rtt_ms);
    EXPECT_DOUBLE_EQ(*snapshot.smoothed.rtt_ms, 500);
    EXPECT_FALSE(snapshot.smoothed.recv_bitrate_bps);
    EXPECT_FALSE(snapshot.smoothed.send_bitrate_bps);
    EXPECT_FALSE(snapshot.smoothed.jitter_ms);
    EXPECT_FALSE(snapshot.smoothed.loss_rate);
    EXPECT_FALSE(controller.OnProcessInterval({5101}).target_rate.estimate.smoothed.loss_rate);
}

TEST(MetricSampling, PathAndAvailabilityChangesClearOldFilteredMetrics)
{
    GoogCcNetworkController controller;
    controller.OnReceiverFeedback(Metrics(1000));
    controller.OnNetworkRouteChange({2000, {}});
    const auto next = controller.OnTransportPacketsFeedback({Packets(2100)});
    EXPECT_FALSE(next.target_rate.estimate.smoothed.rtt_ms);
    EXPECT_FALSE(next.target_rate.estimate.smoothed.jitter_ms);
    controller.OnNetworkAvailability({2200, false});
    EXPECT_FALSE(controller.OnReceiverFeedback(Metrics(2300)).HasUpdates());
    controller.OnNetworkAvailability({2400, true});
    auto feedback = Metrics(2500);
    feedback.rtt_ms = 200;
    const auto recovered = controller.OnReceiverFeedback(feedback);
    ASSERT_TRUE(recovered.target_rate.estimate.smoothed.rtt_ms);
    EXPECT_DOUBLE_EQ(*recovered.target_rate.estimate.smoothed.rtt_ms, 200);
}

TEST(MetricSampling, ReceivingSourcesKeepIndependentFiltersAndRemovalDiscardsHistory)
{
    ReceiveQualityMonitor monitor;
    ASSERT_TRUE(monitor.RegisterSource(1));
    ASSERT_TRUE(monitor.RegisterSource(2));
    monitor.OnFeedback(1, Metrics(1000));
    auto other = Metrics(1000);
    other.rtt_ms = 400;
    monitor.OnFeedback(2, other);
    other.now_ms = 1100;
    const auto first = monitor.OnFeedback(1, other);
    const auto second = monitor.OnFeedback(2, other);
    ASSERT_TRUE(first.target_rate.estimate.smoothed.rtt_ms);
    ASSERT_TRUE(second.target_rate.estimate.smoothed.rtt_ms);
    EXPECT_LT(*first.target_rate.estimate.smoothed.rtt_ms, 150);
    EXPECT_DOUBLE_EQ(*second.target_rate.estimate.smoothed.rtt_ms, 400);
    monitor.Remove(1);
    ASSERT_TRUE(monitor.RegisterSource(1));
    EXPECT_DOUBLE_EQ(*monitor.OnFeedback(1, other).target_rate.estimate.smoothed.rtt_ms, 400);
}

TEST(MetricSampling, SourceRateUsesElapsedTimeAndPreservesLastDemandDuringSilence)
{
    SourceBitrateEstimator slow, fast;
    for (auto* estimator : {&slow, &fast})
    {
        estimator->Observe(1000, 0);
        estimator->Observe(1000, 500);
    }
    slow.Observe(8000, 1500);
    fast.Observe(4000, 1000);
    fast.Observe(4000, 1500);
    EXPECT_NEAR(slow.Bitrate(), fast.Bitrate(), 1);
    EXPECT_GT(slow.Bitrate(), 32000U);
    EXPECT_LT(slow.Bitrate(), 64000U);
    const auto last = slow.Bitrate();
    slow.Observe(1000, 10000);
    EXPECT_EQ(slow.Bitrate(), last);
    slow.Observe(1000, 10500);
    EXPECT_EQ(slow.Bitrate(), 32000U);
}
}
