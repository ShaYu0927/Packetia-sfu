#include <gtest/gtest.h>
#include "transport/MediaTransportBase.h"
#include "SendSideController.h"
#include "RtpSenderTrack.h"
#include "RtpTransportCcExtension.h"
#include "utils.h"
#include <thread>

namespace
{
class FeedbackTransport final : public media::transport::MediaTransportBase
{
public:
    explicit FeedbackTransport(uint64_t id = 1) : MediaTransportBase(id)
    { SetState(MediaTransportState::Connected); }
    MediaTransportProtocol Protocol() const noexcept override
    { return MediaTransportProtocol::Udp; }
    SendResult Send(MediaPacketType, const uint8_t* data, size_t size, bool) override
    {
        if (result == SendResult::Ok) packets.emplace_back(data, data + size);
        return result;
    }
    void Close() override { SetState(MediaTransportState::Closed); }
    void Reconnect() { SetState(MediaTransportState::Connected); }
    void Feedback(const std::vector<uint8_t>& packet, uint64_t at_ms = 2000)
    { PublishPacket(MediaPacketType::Rtcp, packet.data(), packet.size(), at_ms); }
    SendResult result = SendResult::Ok;
    std::vector<std::vector<uint8_t>> packets;
};

rtsp::RtpSenderTrackConfig TrackConfig(uint32_t ssrc, uint8_t extension_id = 3)
{
    rtsp::RtpSenderTrackConfig config;
    config.local_ssrc = ssrc;
    config.transport_cc_extension_id = extension_id;
    return config;
}

std::vector<uint8_t> Rtp(uint16_t seq = 100)
{
    std::vector<uint8_t> packet(112, 0);
    packet[0] = 0x80;
    packet[1] = 96;
    utils::Utils::WriteUint16BE(packet.data() + 2, seq);
    utils::Utils::WriteUint32BE(packet.data() + 8, 1234);
    return packet;
}

uint16_t TransportSequence(const std::vector<uint8_t>& packet)
{
    uint16_t sequence = 0;
    EXPECT_TRUE(rtsp::RtpTransportCcExtension::Read(packet.data(), packet.size(), 3, &sequence));
    return sequence;
}

// Small-delta TWCC with one-bit status vectors. media_ssrc deliberately zero:
// transport feedback belongs to the connection, not a particular audio/video SSRC.
std::vector<uint8_t> Twcc(uint16_t base, const std::vector<bool>& received)
{
    std::vector<uint8_t> packet(20, 0);
    packet[0] = 0x8f;
    packet[1] = 205;
    utils::Utils::WriteUint16BE(packet.data() + 12, base);
    utils::Utils::WriteUint16BE(packet.data() + 14, static_cast<uint16_t>(received.size()));
    packet[18] = 1;
    for (size_t offset = 0; offset < received.size(); offset += 14)
    {
        uint16_t chunk = 0x8000;
        for (size_t i = 0; i < 14 && offset + i < received.size(); ++i)
            if (received[offset + i]) chunk |= static_cast<uint16_t>(1U << (13 - i));
        packet.push_back(static_cast<uint8_t>(chunk >> 8));
        packet.push_back(static_cast<uint8_t>(chunk));
    }
    for (bool got : received) if (got) packet.push_back(40);
    while (packet.size() % 4) packet.push_back(0);
    utils::Utils::WriteUint16BE(packet.data() + 2, static_cast<uint16_t>(packet.size() / 4 - 1));
    return packet;
}

std::vector<uint8_t> Rr(uint32_t ssrc, uint8_t lost = 64)
{
    std::vector<uint8_t> packet(32, 0);
    packet[0] = 0x81;
    packet[1] = 201;
    packet[3] = 7;
    utils::Utils::WriteUint32BE(packet.data() + 8, ssrc);
    packet[12] = lost;
    utils::Utils::WriteUint32BE(packet.data() + 20, 9000);
    return packet;
}
}

TEST(SendSideTransportTest, ActualSendAndWireFeedbackShareOneControllerAcrossTracks)
{
    auto transport = std::make_shared<FeedbackTransport>();
    auto controller = transport->GetSendSideController();
    rtsp::RtpSenderTrack audio(TrackConfig(11), transport);
    rtsp::RtpSenderTrack video(TrackConfig(22), transport);
    const auto packet = Rtp();
    ASSERT_TRUE(audio.InputRtpPacket(packet.data(), packet.size()));
    ASSERT_TRUE(video.InputRtpPacket(packet.data(), packet.size()));
    ASSERT_EQ(transport->packets.size(), 2U);
    EXPECT_EQ(TransportSequence(transport->packets[0]), 0);
    EXPECT_EQ(TransportSequence(transport->packets[1]), 1);
    int updates = 0;
    controller->SetUpdateCallback([&](const media::NetworkControlUpdate& update) {
        ++updates;
        EXPECT_TRUE(update.has_target_rate);
        EXPECT_TRUE(update.has_pacer_config);
        // A reentrant state query verifies the callback runs outside the lock.
        EXPECT_EQ(controller->GetNetworkState().target_rate.target_bitrate_bps,
                  update.target_rate.target_bitrate_bps);
    });
    const auto feedback = Twcc(0, {true, false});
    transport->Feedback(feedback);
    EXPECT_EQ(updates, 1);
    EXPECT_DOUBLE_EQ(controller->GetNetworkState().target_rate.estimate.loss_rate, 0.5);
    transport->Feedback(feedback);
    EXPECT_EQ(updates, 1);
    auto other = std::make_shared<FeedbackTransport>(2);
    other->Feedback(feedback);
    EXPECT_FALSE(other->GetSendSideController()->GetNetworkState().HasUpdates());
    EXPECT_NE(controller, other->GetSendSideController());
}

TEST(SendSideTransportTest, FailedSendIsExcludedAndRetransmissionGetsNewSequence)
{
    auto transport = std::make_shared<FeedbackTransport>();
    rtsp::RtpSenderTrack track(TrackConfig(11), transport);
    auto controller = transport->GetSendSideController();
    auto packet = Rtp();
    transport->result = SendResult::Failed;
    EXPECT_FALSE(track.InputRtpPacket(packet.data(), packet.size()));
    transport->Feedback(Twcc(0, {true}));
    EXPECT_FALSE(controller->GetNetworkState().HasUpdates());
    transport->result = SendResult::Ok;
    ASSERT_TRUE(track.InputRtpPacket(packet.data(), packet.size()));
    track.OnRtcpNack({100});
    ASSERT_EQ(transport->packets.size(), 2U);
    EXPECT_EQ(TransportSequence(transport->packets[0]), 1);
    EXPECT_EQ(TransportSequence(transport->packets[1]), 2);
    transport->Feedback(Twcc(0, {false, true, true}));
    EXPECT_TRUE(controller->GetNetworkState().HasUpdates());
    EXPECT_DOUBLE_EQ(controller->GetNetworkState().target_rate.estimate.loss_rate, 0.0);
}

TEST(SendSideTransportTest, SequenceWrapAndPartialFeedbackMatchExtendedHistory)
{
    auto transport = std::make_shared<FeedbackTransport>();
    auto controller = transport->GetSendSideController();
    controller->SequenceAllocator()->Reset(65535);
    rtsp::RtpSenderTrack track(TrackConfig(11), transport);
    for (uint16_t seq : {100, 101, 102})
    {
        const auto packet = Rtp(seq);
        ASSERT_TRUE(track.InputRtpPacket(packet.data(), packet.size()));
    }
    int updates = 0;
    controller->SetUpdateCallback([&](const auto&) { ++updates; });
    transport->Feedback(Twcc(65535, {true, true}));
    EXPECT_EQ(updates, 1);
    transport->Feedback(Twcc(0, {true, false}));
    EXPECT_EQ(updates, 2);
    EXPECT_DOUBLE_EQ(controller->GetNetworkState().target_rate.estimate.loss_rate, 1.0);
}

TEST(SendSideTransportTest, CloseClearsStateAndOldFeedbackCannotResumeSending)
{
    auto transport = std::make_shared<FeedbackTransport>();
    auto controller = transport->GetSendSideController();
    rtsp::RtpSenderTrack track(TrackConfig(11), transport);
    const auto packet = Rtp();
    ASSERT_TRUE(track.InputRtpPacket(packet.data(), packet.size()));
    transport->Feedback(Twcc(0, {true}));
    ASSERT_TRUE(controller->GetNetworkState().HasUpdates());
    int clears = 0;
    controller->SetUpdateCallback([&](const auto& update) {
        if (!update.HasUpdates()) ++clears;
    });
    transport->Close();
    EXPECT_EQ(clears, 1);
    EXPECT_FALSE(controller->GetNetworkState().HasUpdates());
    EXPECT_FALSE(track.InputRtpPacket(packet.data(), packet.size()));
    transport->Reconnect();
    transport->Feedback(Twcc(0, {true}));
    EXPECT_FALSE(controller->GetNetworkState().HasUpdates());
    ASSERT_TRUE(track.InputRtpPacket(packet.data(), packet.size()));
    EXPECT_EQ(TransportSequence(transport->packets.back()), 1);
    transport->Feedback(Twcc(1, {true}));
    EXPECT_TRUE(controller->GetNetworkState().HasUpdates());
}

TEST(SendSideTransportTest, UnnegotiatedTwccUsesReceiverReportsForRegisteredSendersOnly)
{
    auto transport = std::make_shared<FeedbackTransport>();
    auto controller = transport->GetSendSideController();
    {
        rtsp::RtpSenderTrack track(TrackConfig(11, 0), transport);
        const auto packet = Rtp();
        ASSERT_TRUE(track.InputRtpPacket(packet.data(), packet.size()));
        EXPECT_EQ(transport->packets.back()[0] & 0x10, 0);
        EXPECT_EQ(controller->SequenceAllocator()->PeekNextExtendedSequence(), 0);
        transport->Feedback(Twcc(0, {true}));
        transport->Feedback(Rr(99));
        EXPECT_FALSE(controller->GetNetworkState().HasUpdates());
        transport->Feedback(Rr(11));
        const auto state = controller->GetNetworkState();
        EXPECT_TRUE(state.HasUpdates());
        EXPECT_DOUBLE_EQ(state.target_rate.estimate.loss_rate, 0.25);
        EXPECT_EQ(state.target_rate.estimate.jitter_ms, 100);
    }
    int updates = 0;
    controller->SetUpdateCallback([&](const auto&) { ++updates; });
    transport->Feedback(Rr(11));
    EXPECT_EQ(updates, 0);
}

TEST(SendSideTransportTest, InvalidCompoundDoesNotConsumeTheValidFeedbackPrefix)
{
    auto transport = std::make_shared<FeedbackTransport>();
    rtsp::RtpSenderTrack track(TrackConfig(11), transport);
    const auto packet = Rtp();
    ASSERT_TRUE(track.InputRtpPacket(packet.data(), packet.size()));
    auto feedback = Twcc(0, {true});
    auto invalid = feedback;
    invalid.push_back(0);
    transport->Feedback(invalid);
    EXPECT_FALSE(transport->GetSendSideController()->GetNetworkState().HasUpdates());
    transport->Feedback(feedback);
    EXPECT_TRUE(transport->GetSendSideController()->GetNetworkState().HasUpdates());
}

TEST(SendSideTransportTest, RttSupplementPreservesTwccRateAndTransportLoss)
{
    auto transport = std::make_shared<FeedbackTransport>();
    rtsp::RtpSenderTrack track(TrackConfig(11), transport);
    const auto packet = Rtp();
    ASSERT_TRUE(track.InputRtpPacket(packet.data(), packet.size()));
    transport->Feedback(Twcc(0, {true}));
    auto controller = transport->GetSendSideController();
    const auto before = controller->GetNetworkState();
    media::WeakNetFeedback rr;
    rr.now_ms = 2100;
    rr.rtt_ms = 180;
    rr.loss_rate = 0.5;
    controller->OnReceiverFeedback(rr);
    const auto after = controller->GetNetworkState();
    EXPECT_EQ(after.target_rate.target_bitrate_bps, before.target_rate.target_bitrate_bps);
    EXPECT_EQ(after.target_rate.estimate.rtt_ms, 180);
    EXPECT_DOUBLE_EQ(after.target_rate.estimate.loss_rate, 0.0);
}

TEST(SendSideTransportTest, ReceiverFeedbackWithoutTwccUpdatesLossWithoutRtt)
{
    media::SendSideController controller;
    media::WeakNetFeedback rr;
    rr.now_ms = 2100;
    rr.loss_rate = 0.25;
    controller.OnReceiverFeedback(rr);
    const auto state = controller.GetNetworkState();
    ASSERT_TRUE(state.HasUpdates());
    EXPECT_DOUBLE_EQ(state.target_rate.estimate.loss_rate, 0.25);
}

TEST(SendSideTransportTest, ReceiverFeedbackFallsBackAtTwccTimeout)
{
    media::SendSideController controller;
    media::PacketSendInfo packet;
    packet.transport_sequence = 0;
    packet.send_time_ms = 1000;
    packet.size_bytes = 1200;
    controller.OnPacketSent(packet);
    const auto twcc = Twcc(0, {true});
    ASSERT_TRUE(controller.OnRtcpPacket(twcc.data(), twcc.size(), 2000));
    ASSERT_TRUE(controller.GetNetworkState().HasUpdates());

    media::WeakNetFeedback rr;
    rr.loss_rate = 0.5;
    rr.now_ms = 2999;
    controller.OnReceiverFeedback(rr);
    EXPECT_DOUBLE_EQ(controller.GetNetworkState().target_rate.estimate.loss_rate, 0.0);

    rr.now_ms = 3000;
    controller.OnReceiverFeedback(rr);
    EXPECT_DOUBLE_EQ(controller.GetNetworkState().target_rate.estimate.loss_rate, 0.5);
}

TEST(SendSideTransportTest, ConcurrentAudioVideoHistoryAcceptsTransportWideFeedback)
{
    media::SendSideController controller;
    auto send = [&] {
        for (int i = 0; i < 100; ++i)
        {
            media::PacketSendInfo packet;
            packet.transport_sequence = controller.SequenceAllocator()->Allocate().extended_sequence;
            packet.send_time_ms = 1000 + packet.transport_sequence * 10;
            packet.size_bytes = 1200;
            controller.OnPacketSent(packet);
        }
    };
    std::thread audio(send);
    std::thread video(send);
    audio.join();
    video.join();
    EXPECT_EQ(controller.SequenceAllocator()->PeekNextExtendedSequence(), 200);
    const auto feedback = Twcc(0, std::vector<bool>(200, true));
    EXPECT_TRUE(controller.OnRtcpPacket(feedback.data(), feedback.size(), 3100));
    EXPECT_TRUE(controller.GetNetworkState().HasUpdates());
    EXPECT_DOUBLE_EQ(controller.GetNetworkState().target_rate.estimate.loss_rate, 0.0);
}
