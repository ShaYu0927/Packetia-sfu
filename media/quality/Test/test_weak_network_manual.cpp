#include <gtest/gtest.h>

#include <iostream>

#include "GoogCcNetworkController.h"
#include "NetworkController.h"
#include "PacketHistory.h"
#include "TransportFeedbackAdapter.h"
#include "bwe/loss_based_controller.h"
#include "bwe/remote_bitrate_controller.h"
#include "bwe/transport_bwe_controller.h"

namespace
{
media::PacketSendInfo SentPacket(int64_t transport_sequence,
                                 uint64_t send_time_ms,
                                 uint32_t size_bytes = 1200)
{
    media::PacketSendInfo packet;
    packet.transport_sequence = transport_sequence;
    packet.ssrc = 0x11223344;
    packet.rtp_sequence = static_cast<uint16_t>(transport_sequence);
    packet.send_time_ms = send_time_ms;
    packet.size_bytes = size_bytes;
    packet.media_kind = media::MediaKind::Video;
    return packet;
}

media::TransportFeedback ManualFeedback(std::size_t packet_count,
                                        std::size_t lost_every,
                                        uint64_t first_send_ms,
                                        uint64_t send_interval_ms,
                                        uint64_t one_way_delay_ms)
{
    media::TransportFeedback feedback;
    feedback.feedback_time_ms = first_send_ms + packet_count * send_interval_ms +
                                one_way_delay_ms;
    feedback.packet_feedbacks.reserve(packet_count);
    for (std::size_t i = 0; i < packet_count; ++i)
    {
        media::PacketFeedback item;
        item.sent_packet = SentPacket(static_cast<int64_t>(i),
                                      first_send_ms + i * send_interval_ms);
        item.received = lost_every == 0 || (i + 1) % lost_every != 0;
        if (item.received)
        {
            item.receive_time_ms = item.sent_packet.send_time_ms + one_way_delay_ms;
        }
        feedback.packet_feedbacks.push_back(item);
    }
    return feedback;
}
} // namespace

TEST(WeakNetworkManualTest, TransportFeedbackCalculatesLossAndDelay)
{
    const auto feedback = ManualFeedback(10, 5, 1000, 20, 50);
    std::cout << "[WEAK_NET_TEST][FEEDBACK] packets=" << feedback.PacketCount()
              << " received=" << feedback.ReceivedCount()
              << " lost=" << feedback.LostCount()
              << " loss_rate=" << feedback.LossRate()
              << " first_delay_ms=" << feedback.packet_feedbacks.front().DelayMs() << '\n';
    EXPECT_EQ(feedback.PacketCount(), 10U);
    EXPECT_EQ(feedback.ReceivedCount(), 8U);
    EXPECT_EQ(feedback.LostCount(), 2U);
    EXPECT_DOUBLE_EQ(feedback.LossRate(), 0.2);
    EXPECT_EQ(feedback.packet_feedbacks.front().DelayMs(), 50U);
    EXPECT_EQ(feedback.packet_feedbacks[4].DelayMs(), 0U);
}

TEST(WeakNetworkManualTest, PacketHistoryIsBoundedAndUpdatesExistingPacket)
{
    media::PacketHistory history(2);
    history.AddPacket(SentPacket(10, 100));
    history.AddPacket(SentPacket(11, 120));

    auto updated = SentPacket(11, 125, 1300);
    history.AddPacket(updated);
    history.AddPacket(SentPacket(12, 140));

    media::PacketSendInfo output;
    const bool has_evicted = history.GetPacket(10, output);
    const bool has_updated = history.GetPacket(11, output);
    std::cout << "[WEAK_NET_TEST][HISTORY] capacity=2"
              << " seq10_present=" << has_evicted
              << " seq11_present=" << has_updated
              << " seq11_send_ms=" << output.send_time_ms
              << " seq11_bytes=" << output.size_bytes << '\n';
    EXPECT_FALSE(has_evicted);
    ASSERT_TRUE(has_updated);
    EXPECT_EQ(output.send_time_ms, 125U);
    EXPECT_EQ(output.size_bytes, 1300U);
    EXPECT_TRUE(history.GetPacket(12, output));

    history.Clear();
    EXPECT_FALSE(history.GetPacket(11, output));
}

TEST(WeakNetworkManualTest, FeedbackAdapterMatchesHistoryAndSkipsUnknownPackets)
{
    media::PacketHistory history;
    history.AddPacket(SentPacket(100, 1000));
    history.AddPacket(SentPacket(101, 1020));

    rtcpx::TransportFeedbackReport report;
    report.packets.push_back({100, true, 1050000, 1050});
    report.packets.push_back({101, false, 0, 0});
    report.packets.push_back({102, true, 1090000, 1090}); // Not in send history.

    media::TransportFeedbackAdapter adapter;
    const auto feedback = adapter.Build(report, history, 1100);
    std::cout << "[WEAK_NET_TEST][ADAPTER] report_packets=" << report.packets.size()
              << " matched_packets=" << feedback.packet_feedbacks.size()
              << " loss_rate=" << feedback.LossRate() << '\n';
    ASSERT_EQ(feedback.packet_feedbacks.size(), 2U);
    EXPECT_TRUE(feedback.packet_feedbacks[0].received);
    EXPECT_EQ(feedback.packet_feedbacks[0].DelayMs(), 50U);
    EXPECT_FALSE(feedback.packet_feedbacks[1].received);
    EXPECT_DOUBLE_EQ(feedback.LossRate(), 0.5);
}

TEST(WeakNetworkManualTest, LossControllerIncreasesHoldsAndDecreases)
{
    LossBasedController controller;
    const uint32_t no_loss = controller.Update(1000000, 0.0);
    const uint32_t medium_loss = controller.Update(1000000, 0.05);
    const uint32_t high_loss = controller.Update(1000000, 0.20);
    std::cout << "[WEAK_NET_TEST][LOSS] input_bps=1000000"
              << " loss0_output=" << no_loss
              << " loss5_output=" << medium_loss
              << " loss20_output=" << high_loss << '\n';
    EXPECT_EQ(no_loss, 1050000U);
    EXPECT_EQ(medium_loss, 1000000U);
    EXPECT_EQ(high_loss, 900000U);
}

TEST(WeakNetworkManualTest, RemoteBitrateControllerReactsToManualUsageAndLoss)
{
    RemoteBitrateController controller;
    const auto normal = controller.Update(BandwidthUsage::kNormal, 600000, 0.0, 1000);
    EXPECT_EQ(normal.usage, BandwidthUsage::kNormal);
    EXPECT_EQ(normal.rc_state, RateControlState::kIncrease);
    EXPECT_EQ(normal.target_bitrate_bps, 551250U);

    const auto congested = controller.Update(BandwidthUsage::kOverUsing,
                                             400000, 0.20, 1100);
    std::cout << "[WEAK_NET_TEST][REMOTE_BWE] normal_target_bps="
              << normal.target_bitrate_bps
              << " congested_target_bps=" << congested.target_bitrate_bps
              << " congested_loss_rate=" << congested.loss_rate << '\n';
    EXPECT_EQ(congested.usage, BandwidthUsage::kOverUsing);
    EXPECT_EQ(congested.rc_state, RateControlState::kDecrease);
    EXPECT_EQ(congested.target_bitrate_bps, 306000U);
}

TEST(WeakNetworkManualTest, NetworkPolicyClassifiesQualityAndThrottlesPli)
{
    media::NetworkController controller;

    media::WeakNetFeedback excellent;
    excellent.now_ms = 1000;
    excellent.send_bitrate_bps = 800000;
    excellent.receive_bitrate_bps = 780000;
    auto update = controller.OnFeedback(excellent);
    EXPECT_EQ(controller.GetSnapshot().quality, media::NetworkQualityLevel::Excellent);
    EXPECT_TRUE(update.has_target_rate);
    EXPECT_TRUE(update.has_pacer_config);
    EXPECT_EQ(update.pacer_config.pacing_bitrate_bps,
              update.target_rate.target_bitrate_bps * 12 / 10);
    EXPECT_FALSE(update.request_key_frame);
    EXPECT_FALSE(update.enable_fec);

    media::WeakNetFeedback bad = excellent;
    bad.now_ms = 2000;
    bad.loss_rate = 0.25;
    bad.rtt_ms = 600;
    update = controller.OnFeedback(bad);
    const auto first_bad = update;
    EXPECT_EQ(controller.GetSnapshot().quality, media::NetworkQualityLevel::Bad);
    EXPECT_TRUE(update.enable_fec);
    EXPECT_TRUE(update.request_key_frame);

    bad.now_ms = 2500;
    const auto throttled = controller.OnFeedback(bad);
    EXPECT_FALSE(throttled.request_key_frame);
    bad.now_ms = 3000;
    const auto allowed_again = controller.OnFeedback(bad);
    EXPECT_TRUE(allowed_again.request_key_frame);
    std::cout << "[WEAK_NET_TEST][POLICY] quality="
              << media::ToString(controller.GetSnapshot().quality)
              << " target_bps=" << first_bad.target_rate.target_bitrate_bps
              << " pacer_bps=" << first_bad.pacer_config.pacing_bitrate_bps
              << " fec=" << first_bad.enable_fec
              << " pli_at_2000=" << first_bad.request_key_frame
              << " pli_at_2500=" << throttled.request_key_frame
              << " pli_at_3000=" << allowed_again.request_key_frame << '\n';
}

TEST(WeakNetworkManualTest, GoogCcIgnoresFeedbackWhileNetworkIsUnavailable)
{
    media::GoogCcNetworkController controller;
    const auto feedback = ManualFeedback(20, 0, 1000, 20, 50);

    media::TransportPacketsFeedbackMessage message;
    message.feedback = feedback;
    const auto active = controller.OnTransportPacketsFeedback(message);
    EXPECT_TRUE(active.has_target_rate);
    EXPECT_TRUE(active.has_pacer_config);

    media::NetworkAvailability unavailable;
    unavailable.at_time_ms = 1600;
    unavailable.network_available = false;
    const auto stopped = controller.OnNetworkAvailability(unavailable);
    const auto ignored = controller.OnTransportPacketsFeedback(message);
    const auto state = controller.GetNetworkState(1700);
    std::cout << "[WEAK_NET_TEST][AVAILABILITY] active_updates=" << active.HasUpdates()
              << " stop_updates=" << stopped.HasUpdates()
              << " feedback_while_down_updates=" << ignored.HasUpdates()
              << " state_while_down_updates=" << state.HasUpdates() << '\n';
    EXPECT_FALSE(stopped.HasUpdates());
    EXPECT_FALSE(ignored.HasUpdates());
    EXPECT_FALSE(state.HasUpdates());
}

TEST(WeakNetworkManualTest, TransportBweUsesOnlyReceivedPacketsAndReportsLoss)
{
    TransportBweController controller;
    const auto feedback = ManualFeedback(20, 4, 1000, 20, 50);
    const auto result = controller.OnTransportFeedback(feedback);
    std::cout << "[WEAK_NET_TEST][TRANSPORT_BWE] packets=" << feedback.PacketCount()
              << " received=" << feedback.ReceivedCount()
              << " loss_rate=" << result.loss_rate
              << " target_bps=" << result.target_bitrate_bps
              << " delay_trend_ms=" << result.delay_trend_ms << '\n';

    EXPECT_EQ(result.loss_rate, 0.25);
    EXPECT_GT(result.target_bitrate_bps, 0U);
    EXPECT_GE(result.target_bitrate_bps, 80000U);
    EXPECT_LE(result.target_bitrate_bps, 2000000U);
}
