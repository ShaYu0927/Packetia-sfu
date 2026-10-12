#include <gtest/gtest.h>
#include <algorithm>
#include <cmath>
#include <limits>
#include "utils.h"
#include "GoogCcNetworkController.h"
#include "SendSideController.h"
#include "bwe/remote_bitrate_controller.h"
#include "bwe/ou_trend_detect.h"
#include "bwe/incoming_bitrate_estimator.h"

namespace
{
void Initialize(RemoteBitrateController& controller, uint32_t rtt = 100)
{
    controller.SetBitrateConstraints(100000, 500000, 10000000);
    controller.Update(BandwidthUsage::kNormal, 800000, 0, 0, rtt);
    const auto ready = controller.Update(BandwidthUsage::kNormal, 800000, 0, 2000, rtt);
    ASSERT_TRUE(ready.estimate_valid);
    ASSERT_EQ(ready.target_bitrate_bps, 800000U);
}

media::TransportFeedback Feedback(uint64_t first_send, uint64_t receive_offset,
                                  uint64_t feedback_time, bool all_lost = false)
{
    media::TransportFeedback feedback;
    feedback.feedback_time_ms = feedback_time;
    for (int i = 0; i < 30; ++i) {
        media::PacketFeedback packet;
        packet.sent_packet.transport_sequence = i;
        packet.sent_packet.send_time_ms = first_send + i * 20;
        packet.sent_packet.size_bytes = 1200;
        packet.received = !all_lost;
        packet.receive_time_ms = packet.sent_packet.send_time_ms + receive_offset;
        feedback.packet_feedbacks.push_back(packet);
    }
    return feedback;
}

// Real TWCC bytes for received packets with small deltas, fed through history
// matching and RTCP parsing rather than directly calling the estimator.
void FeedWire(media::SendSideController& controller, media::TransportFeedback feedback)
{
    for (auto& packet : feedback.packet_feedbacks) {
        packet.sent_packet.transport_sequence = controller.SequenceAllocator()->Allocate().extended_sequence;
        controller.OnPacketSent(packet.sent_packet);
    }
    const auto& packets = feedback.packet_feedbacks;
    std::vector<uint8_t> wire(20, 0);
    wire[0] = 0x8f;
    wire[1] = 205;
    utils::Utils::WriteUint16BE(wire.data() + 12, static_cast<uint16_t>(packets.front().sent_packet.transport_sequence));
    utils::Utils::WriteUint16BE(wire.data() + 14, static_cast<uint16_t>(packets.size()));
    const uint64_t reference = packets.front().receive_time_ms / 64;
    wire[16] = static_cast<uint8_t>(reference >> 16);
    wire[17] = static_cast<uint8_t>(reference >> 8);
    wire[18] = static_cast<uint8_t>(reference);
    for (size_t offset = 0; offset < packets.size(); offset += 14) {
        uint16_t chunk = 0x8000;
        for (size_t i = 0; i < 14 && offset + i < packets.size(); ++i)
            chunk |= static_cast<uint16_t>(1U << (13 - i));
        wire.push_back(static_cast<uint8_t>(chunk >> 8));
        wire.push_back(static_cast<uint8_t>(chunk));
    }
    uint64_t previous_arrival = reference * 64;
    for (const auto& packet : packets) {
        ASSERT_TRUE(packet.received);
        const auto delta = (packet.receive_time_ms - previous_arrival) * 4;
        ASSERT_LE(delta, 255U);
        wire.push_back(static_cast<uint8_t>(delta));
        previous_arrival = packet.receive_time_ms;
    }
    while (wire.size() % 4) wire.push_back(0);
    utils::Utils::WriteUint16BE(wire.data() + 2, static_cast<uint16_t>(wire.size() / 4 - 1));
    ASSERT_TRUE(controller.OnRtcpPacket(wire.data(), wire.size(), feedback.feedback_time_ms));
}
}

TEST(BweRateControlTest, StartupUsesConfiguredRateUntilReceiveEstimateIsReady)
{
    RemoteBitrateController controller;
    controller.SetBitrateConstraints(20000, 64000, 128000);
    EXPECT_EQ(controller.Update(BandwidthUsage::kNormal, 0, 0, 0).target_bitrate_bps, 64000U);
    EXPECT_FALSE(controller.Update(BandwidthUsage::kNormal, 48000, 0, 100).estimate_valid);
    const auto ready = controller.Update(BandwidthUsage::kNormal, 48000, 0, 2100);
    EXPECT_TRUE(ready.estimate_valid);
    EXPECT_EQ(ready.target_bitrate_bps, 48000U);
}

TEST(BweRateControlTest, GrowthDependsOnElapsedTimeNotFeedbackCount)
{
    RemoteBitrateController slow, fast;
    Initialize(slow);
    Initialize(fast);
    BweResult a, b;
    for (int t = 2100; t <= 3000; t += 100) a = slow.Update(BandwidthUsage::kNormal, 800000, 0, t);
    for (int t = 2010; t <= 3000; t += 10) b = fast.Update(BandwidthUsage::kNormal, 800000, 0, t);
    EXPECT_GT(a.target_bitrate_bps, 800000U);
    EXPECT_NEAR(a.target_bitrate_bps, b.target_bitrate_bps, 2.0);
    const auto duplicate = fast.Update(BandwidthUsage::kNormal, 800000, 0, 3000);
    const auto backwards = fast.Update(BandwidthUsage::kNormal, 800000, 0, 2999);
    EXPECT_EQ(duplicate.target_bitrate_bps, b.target_bitrate_bps);
    EXPECT_EQ(backwards.target_bitrate_bps, b.target_bitrate_bps);
}

TEST(BweRateControlTest, HighRttSlowsIncreaseAndUnderuseHolds)
{
    RemoteBitrateController near, far;
    Initialize(near, 20);
    Initialize(far, 800);
    const auto fast = near.Update(BandwidthUsage::kNormal, 800000, 0, 3000);
    const auto slow = far.Update(BandwidthUsage::kNormal, 800000, 0, 3000);
    EXPECT_GT(fast.target_bitrate_bps, slow.target_bitrate_bps);
    EXPECT_EQ(near.Update(BandwidthUsage::kUnderUsing, 800000, 0, 4000).target_bitrate_bps,
              fast.target_bitrate_bps);
}

TEST(BweRateControlTest, LowRatesFullLossAndNonfiniteLossCannotWrapOrDivideByZero)
{
    RemoteBitrateController controller;
    controller.SetBitrateConstraints(1000, 1500, 10000);
    const auto congested = controller.Update(BandwidthUsage::kOverUsing, 500, 1, 0);
    EXPECT_EQ(congested.target_bitrate_bps, 1000U);
    for (int t = 100; t <= 5000; t += 100) {
        const auto result = controller.Update(BandwidthUsage::kNormal, 0,
            std::numeric_limits<double>::quiet_NaN(), t);
        EXPECT_GE(result.target_bitrate_bps, 1000U);
        EXPECT_LE(result.target_bitrate_bps, 1500U);
        EXPECT_DOUBLE_EQ(result.loss_rate, 1.0);
    }
    EXPECT_THROW(controller.SetBitrateConstraints(0, 1, 2), std::invalid_argument);
    EXPECT_THROW(controller.SetBitrateConstraints(10, 1, 20), std::invalid_argument);
}

TEST(BweRateControlTest, LossObservationHoldsBeforeRecheckingAndRecoversConservatively)
{
    RemoteBitrateController controller;
    Initialize(controller);
    const auto reduced = controller.Update(BandwidthUsage::kNormal, 800000, 0.2, 2100);
    EXPECT_LT(reduced.target_bitrate_bps, 800000U);
    const auto waiting = controller.Update(BandwidthUsage::kNormal, 800000, 0.2, 2200);
    EXPECT_EQ(waiting.target_bitrate_bps, reduced.target_bitrate_bps);
    const auto recovered = controller.Update(BandwidthUsage::kNormal, 800000, 0.02, 3600);
    EXPECT_GT(recovered.target_bitrate_bps, reduced.target_bitrate_bps);
    EXPECT_LE(recovered.target_bitrate_bps, 800000U);
}

TEST(BweRateControlTest, WarmupCannotUndoAnActiveLossReduction)
{
    RemoteBitrateController controller;
    controller.SetBitrateConstraints(100000, 500000, 2000000);
    controller.Update(BandwidthUsage::kNormal, 500000, 0.2, 0);
    for (int t = 100; t < 1900; t += 100)
        controller.Update(BandwidthUsage::kNormal, 500000, 0.2, t);
    const auto before = controller.Update(BandwidthUsage::kNormal, 500000, 0.2, 1900);
    const auto ready = controller.Update(BandwidthUsage::kNormal, 500000, 0.2, 2000);
    ASSERT_TRUE(ready.estimate_valid);
    EXPECT_LE(ready.target_bitrate_bps, before.target_bitrate_bps);
}

TEST(BweRateControlTest, DelayCongestionCancelsStaleLossRecovery)
{
    RemoteBitrateController controller;
    Initialize(controller);
    controller.Update(BandwidthUsage::kNormal, 800000, 0.2, 2100);
    const auto congested = controller.Update(BandwidthUsage::kOverUsing, 400000, 0.2, 2200);
    const auto later = controller.Update(BandwidthUsage::kUnderUsing, 400000, 0.2, 3600);
    EXPECT_LE(later.target_bitrate_bps, congested.target_bitrate_bps);
}

TEST(BweRateControlTest, DelayDetectsPersistentQueueThenResetsAndHandlesWrap)
{
    OUTrendDetect detector;
    detector.SetNetworkEstimate(800000, 100);
    BandwidthUsage state = BandwidthUsage::kNormal;
    for (int i = 0; i < 50; ++i) state = detector.OnPacket(1000 + 20 * i, 2000 + 35 * i, 1200);
    EXPECT_EQ(state, BandwidthUsage::kOverUsing);
    EXPECT_GT(detector.effective_trend_ms(), 35);
    detector.Reset();
    for (int i = 0; i < 50; ++i) state = detector.OnPacket(1000 + 20 * i, 9000 + 20 * i, 1200);
    EXPECT_EQ(state, BandwidthUsage::kNormal);
    detector.Reset();
    detector.OnPacket(UINT32_MAX - 10, 1000, 1200);
    EXPECT_EQ(detector.OnPacket(9, 1020, 1200), BandwidthUsage::kNormal);
    const double before = detector.trend_ms();
    detector.OnPacket(8, 1019, 1200);
    EXPECT_DOUBLE_EQ(detector.trend_ms(), before);
}

TEST(BweRateControlTest, ReceiveRateHasWarmupAndReset)
{
    IncomingBitrateEstimator estimator;
    EXPECT_EQ(estimator.Update(1200, 0), 0U);
    for (int t = 20; t <= 500; t += 20) estimator.Update(1200, t);
    EXPECT_NEAR(estimator.Update(1200, 520), 480000, 30000);
    estimator.Reset();
    EXPECT_EQ(estimator.Update(1200, 10000), 0U);
    estimator.Reset();
    estimator.Update(std::numeric_limits<size_t>::max(), 0);
    EXPECT_EQ(estimator.Update(std::numeric_limits<size_t>::max(), 100), UINT32_MAX);
}

TEST(BweRateControlTest, IsolatedJitterIsToleratedAndExpiresFromHistory)
{
    OUTrendDetect detector;
    detector.OnPacket(0, 1000, 1200);
    EXPECT_EQ(detector.OnPacket(20, 1220, 1200), BandwidthUsage::kNormal);
    EXPECT_DOUBLE_EQ(detector.max_jitter_ms(), 200);
    detector.OnPacket(1040, 2240, 1200);
    detector.OnPacket(10040, 11240, 1200);
    EXPECT_DOUBLE_EQ(detector.max_jitter_ms(), 200);
    EXPECT_EQ(detector.OnPacket(11040, 12240, 1200), BandwidthUsage::kNormal);
    EXPECT_DOUBLE_EQ(detector.max_jitter_ms(), 30);
}

TEST(BweRateControlTest, WireTwccReducesRateForQueueingAndRecoversOnStablePath)
{
    media::SendSideController controller;
    FeedWire(controller, Feedback(1000, 30000, 1700));
    FeedWire(controller, Feedback(4000, 30000, 4700));
    const auto normal = controller.GetNetworkState().target_rate.target_bitrate_bps;
    auto queued = Feedback(5000, 30000, 6200);
    for (size_t i = 0; i < queued.packet_feedbacks.size(); ++i)
        queued.packet_feedbacks[i].receive_time_ms = 35000 + i * 35;
    FeedWire(controller, queued);
    const auto congested = controller.GetNetworkState();
    EXPECT_LT(congested.target_rate.target_bitrate_bps, normal);
    EXPECT_GT(congested.target_rate.estimate.recv_bitrate_bps, 0U);
    EXPECT_DOUBLE_EQ(congested.target_rate.estimate.loss_rate, 0);
    uint32_t lowest = congested.target_rate.target_bitrate_bps;
    for (int batch = 0; batch < 12; ++batch) {
        FeedWire(controller, Feedback(6000 + batch * 600, 30435, 7000 + batch * 600));
        lowest = std::min(lowest, controller.GetNetworkState().target_rate.target_bitrate_bps);
    }
    EXPECT_GT(controller.GetNetworkState().target_rate.target_bitrate_bps, lowest);
    EXPECT_GE(lowest, 100000U);
}

TEST(BweRateControlTest, ReceiverRttSlowsSubsequentWireTwccGrowth)
{
    for (bool rr_before_twcc : {false, true}) {
        media::SendSideController near, far;
        media::WeakNetFeedback rr;
        if (!rr_before_twcc) {
            FeedWire(near, Feedback(1000, 30000, 1700));
            FeedWire(far, Feedback(1000, 30000, 1700));
        }
        rr.now_ms = rr_before_twcc ? 500 : 1701;
        rr.rtt_ms = 20;
        near.OnReceiverFeedback(rr);
        rr.rtt_ms = 800;
        far.OnReceiverFeedback(rr);
        if (rr_before_twcc) {
            FeedWire(near, Feedback(1000, 30000, 1700));
            FeedWire(far, Feedback(1000, 30000, 1700));
        }
        FeedWire(near, Feedback(4000, 30000, 4700));
        FeedWire(far, Feedback(4000, 30000, 4700));
        FeedWire(near, Feedback(5000, 30000, 5700));
        FeedWire(far, Feedback(5000, 30000, 5700));
        EXPECT_GT(near.GetNetworkState().target_rate.target_bitrate_bps,
                  far.GetNetworkState().target_rate.target_bitrate_bps);
        EXPECT_EQ(far.GetNetworkState().target_rate.estimate.rtt_ms, 800U);
    }
}

TEST(BweRateControlTest, TransportUsesConstraintsAndSeparatesMeasuredRateFromStartupTarget)
{
    media::NetworkControllerConfig config;
    config.bitrate = {20000, 64000, 128000};
    media::GoogCcNetworkController controller(config);
    const auto update = controller.OnTransportPacketsFeedback({Feedback(1000, 30000, 1700)});
    ASSERT_TRUE(update.has_target_rate);
    EXPECT_EQ(update.target_rate.target_bitrate_bps, 64000U);
    EXPECT_EQ(update.target_rate.stable_target_bitrate_bps, 64000U);
    EXPECT_GT(update.target_rate.estimate.recv_bitrate_bps, 400000U);
    EXPECT_EQ(update.target_rate.estimate.bandwidth_bps, 0U);
    const auto established = controller.OnTransportPacketsFeedback({Feedback(4000, 30000, 4700)});
    EXPECT_EQ(established.target_rate.target_bitrate_bps, 128000U);
    EXPECT_EQ(established.target_rate.estimate.bandwidth_bps, 128000U);
}

TEST(BweRateControlTest, RouteAndNetworkResetMatchFreshController)
{
    media::GoogCcNetworkController old;
    old.OnRoundTripTimeUpdate({0, 800});
    old.OnTransportPacketsFeedback({Feedback(1000, 30000, 1700)});
    old.OnTransportPacketsFeedback({Feedback(4000, 30000, 4700, true)});
    media::BitrateConstraints next{10000, 32000, 128000};
    EXPECT_FALSE(old.OnNetworkRouteChange({5000, next}).HasUpdates());
    media::NetworkControllerConfig config;
    config.bitrate = next;
    media::GoogCcNetworkController fresh(config);
    const auto feedback = Feedback(6000, 0, 6700);
    const auto a = old.OnTransportPacketsFeedback({feedback});
    const auto b = fresh.OnTransportPacketsFeedback({feedback});
    EXPECT_EQ(a.target_rate.target_bitrate_bps, b.target_rate.target_bitrate_bps);
    EXPECT_EQ(a.target_rate.estimate.recv_bitrate_bps, b.target_rate.estimate.recv_bitrate_bps);
    EXPECT_EQ(a.target_rate.estimate.rtt_ms, 0U);
    EXPECT_FALSE(old.OnNetworkAvailability({7000, false}).HasUpdates());
    media::WeakNetFeedback stale;
    stale.now_ms = 7050;
    stale.rtt_ms = 800;
    EXPECT_FALSE(old.OnReceiverFeedback(stale).HasUpdates());
    EXPECT_FALSE(old.OnRoundTripTimeUpdate({7060, 800}).HasUpdates());
    old.OnNetworkAvailability({7100, true});
    EXPECT_FALSE(old.OnProcessInterval({7101}).HasUpdates());
    const auto reset = old.OnTransportPacketsFeedback({Feedback(8000, 0, 8700)});
    EXPECT_EQ(reset.target_rate.target_bitrate_bps, 32000U);
    EXPECT_EQ(reset.target_rate.estimate.bandwidth_bps, 0U);
    EXPECT_EQ(reset.target_rate.estimate.rtt_ms, 0U);
}

TEST(BweRateControlTest, PacerSuggestionSaturatesAtLargeConfiguredRate)
{
    media::NetworkControllerConfig config;
    config.bitrate = {100000, UINT32_MAX, UINT32_MAX};
    media::GoogCcNetworkController controller(config);
    const auto update = controller.OnTransportPacketsFeedback({Feedback(1000, 0, 1700)});
    EXPECT_EQ(update.target_rate.target_bitrate_bps, UINT32_MAX);
    EXPECT_EQ(update.pacer_config.pacing_bitrate_bps, UINT32_MAX);
    const auto fallback = controller.OnReceiverFeedback({});
    EXPECT_EQ(fallback.target_rate.target_bitrate_bps, UINT32_MAX);
    EXPECT_EQ(fallback.pacer_config.pacing_bitrate_bps, UINT32_MAX);
}
