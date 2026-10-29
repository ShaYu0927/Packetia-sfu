#include <gtest/gtest.h>

#include "RtcpReciver.h"
#include "SendSideController.h"
#include "utils.h"

namespace
{
class TwccObserver final : public rtcpx::IRtcpObserver
{
public:
    std::vector<rtcpx::TransportFeedbackReport> reports;
    size_t receiver_reports = 0;
    void OnTransportFeedback(const rtcpx::TransportFeedbackReport& report) override
    { reports.push_back(report); }
    void OnReceiverReport(uint32_t, uint32_t, uint8_t, int32_t, uint32_t,
                          uint32_t, uint32_t, uint32_t) override { ++receiver_reports; }
    void OnSenderReport(uint32_t, uint64_t, uint32_t, uint32_t, uint32_t) override {}
    void OnNack(uint32_t, uint32_t, const uint16_t*, size_t) override {}
    void OnPli(uint32_t, uint32_t) override {}
    void OnFir(uint32_t, uint32_t, uint8_t) override {}
    void OnBye(uint32_t) override {}
    void OnRttUpdated(uint32_t, uint32_t) override {}
};

// Two-bit status vector; every received packet uses a signed large delta.
std::vector<uint8_t> SignedTwcc(uint32_t reference_ticks,
                               const std::vector<uint8_t>& statuses,
                               const std::vector<int16_t>& deltas)
{
    std::vector<uint8_t> packet(22, 0);
    packet[0] = 0x8f;
    packet[1] = 205;
    utils::Utils::WriteUint16BE(packet.data() + 14, static_cast<uint16_t>(statuses.size()));
    utils::Utils::WriteUint24BE(packet.data() + 16, reference_ticks);
    uint16_t chunk = 0xc000;
    for (size_t i = 0; i < statuses.size(); ++i)
        chunk |= static_cast<uint16_t>(statuses[i] << (12 - 2 * i));
    utils::Utils::WriteUint16BE(packet.data() + 20, chunk);
    for (int16_t delta : deltas)
    {
        packet.push_back(static_cast<uint8_t>(static_cast<uint16_t>(delta) >> 8));
        packet.push_back(static_cast<uint8_t>(delta));
    }
    while (packet.size() % 4) packet.push_back(0);
    utils::Utils::WriteUint16BE(packet.data() + 2, static_cast<uint16_t>(packet.size() / 4 - 1));
    return packet;
}
}

TEST(TwccFeedbackTest, NegativeDeltaIsAppliedBeforeConvertingToMilliseconds)
{
    TwccObserver observer;
    rtcpx::RtcpReceiverImpl receiver(&observer);
    const auto packet = SignedTwcc(2, {2, 0, 2}, {-41, 12});
    ASSERT_TRUE(receiver.OnRtcpPacket(packet.data(), packet.size()));
    ASSERT_EQ(observer.reports.size(), 1U);
    const auto& packets = observer.reports[0].packets;
    ASSERT_EQ(packets.size(), 3U);
    EXPECT_EQ(packets[0].receive_time_us, 117750);
    EXPECT_EQ(packets[0].receive_time_ms, 117U);
    EXPECT_FALSE(packets[1].received);
    EXPECT_EQ(packets[2].receive_time_us, 120750);
    EXPECT_EQ(packets[2].receive_time_ms, 120U);
}

TEST(TwccFeedbackTest, ReorderedArrivalsPreserveSignedDeltas)
{
    TwccObserver observer;
    rtcpx::RtcpReceiverImpl receiver(&observer);
    const auto packet = SignedTwcc(1, {2, 2, 2}, {40, -80, 120});
    ASSERT_TRUE(receiver.OnRtcpPacket(packet.data(), packet.size()));
    ASSERT_EQ(observer.reports.size(), 1U);
    const auto& packets = observer.reports[0].packets;
    ASSERT_EQ(packets.size(), 3U);
    EXPECT_EQ(packets[0].receive_time_ms, 74U);
    EXPECT_EQ(packets[1].receive_time_ms, 54U);
    EXPECT_EQ(packets[2].receive_time_ms, 84U);
}

TEST(TwccFeedbackTest, NegativeTimestampDoesNotUnderflowUnsignedMilliseconds)
{
    TwccObserver observer;
    rtcpx::RtcpReceiverImpl receiver(&observer);
    const auto packet = SignedTwcc(0, {2, 2}, {-40, 80});
    ASSERT_TRUE(receiver.OnRtcpPacket(packet.data(), packet.size()));
    ASSERT_EQ(observer.reports.size(), 1U);
    EXPECT_EQ(observer.reports[0].packets[0].receive_time_us, -10000);
    EXPECT_EQ(observer.reports[0].packets[0].receive_time_ms, 0U);
    EXPECT_EQ(observer.reports[0].packets[1].receive_time_ms, 10U);
}

TEST(TwccFeedbackTest, InvalidTwccPayloadRejectsCompoundBeforeDispatchingPrefix)
{
    for (const auto& invalid : {SignedTwcc(1, {2, 2, 2}, {40}),
                                SignedTwcc(1, {3}, {})})
    {
        TwccObserver observer;
        rtcpx::RtcpReceiverImpl receiver(&observer);
        auto packet = SignedTwcc(1, {2}, {40});
        packet.insert(packet.end(), invalid.begin(), invalid.end());
        EXPECT_FALSE(receiver.OnRtcpPacket(packet.data(), packet.size()));
        EXPECT_TRUE(observer.reports.empty());
    }
}

TEST(TwccFeedbackTest, InvalidFramingRejectsCompoundBeforeDispatchingPrefix)
{
    TwccObserver observer;
    rtcpx::RtcpReceiverImpl receiver(&observer);
    auto packet = SignedTwcc(1, {2}, {40});
    packet.push_back(0);
    EXPECT_FALSE(receiver.OnRtcpPacket(packet.data(), packet.size()));
    EXPECT_TRUE(observer.reports.empty());
}

TEST(TwccFeedbackTest, ValidCompoundDispatchesEachTwccReportOnce)
{
    TwccObserver observer;
    rtcpx::RtcpReceiverImpl receiver(&observer);
    auto packet = SignedTwcc(1, {2}, {40});
    const auto second = SignedTwcc(2, {2}, {-40});
    packet.insert(packet.end(), second.begin(), second.end());
    ASSERT_TRUE(receiver.OnRtcpPacket(packet.data(), packet.size()));
    ASSERT_EQ(observer.reports.size(), 2U);
    EXPECT_EQ(observer.reports[0].packets[0].receive_time_ms, 74U);
    EXPECT_EQ(observer.reports[1].packets[0].receive_time_ms, 118U);
}

TEST(TwccFeedbackTest, InvalidTwccDoesNotPublishReceiverReportPrefix)
{
    TwccObserver observer;
    rtcpx::RtcpReceiverImpl receiver(&observer);
    std::vector<uint8_t> packet(32, 0);
    packet[0] = 0x81;
    packet[1] = 201;
    packet[3] = 7;
    const auto invalid = SignedTwcc(1, {2, 2, 2}, {40});
    auto compound = packet;
    compound.insert(compound.end(), invalid.begin(), invalid.end());
    EXPECT_FALSE(receiver.OnRtcpPacket(compound.data(), compound.size()));
    EXPECT_EQ(observer.receiver_reports, 0U);
    ASSERT_TRUE(receiver.OnRtcpPacket(packet.data(), packet.size()));
    EXPECT_EQ(observer.receiver_reports, 1U);
}

TEST(TwccFeedbackTest, ShortTwccHeaderIsRejected)
{
    TwccObserver observer;
    rtcpx::RtcpReceiverImpl receiver(&observer);
    std::vector<uint8_t> packet(12, 0);
    packet[0] = 0x8f;
    packet[1] = 205;
    packet[3] = 2;
    EXPECT_FALSE(receiver.OnRtcpPacket(packet.data(), packet.size()));
    EXPECT_TRUE(observer.reports.empty());
}

TEST(TwccFeedbackTest, InvalidCompoundDoesNotConsumeSendHistoryOrNotifyController)
{
    media::SendSideController controller;
    media::PacketSendInfo sent;
    sent.transport_sequence = 0;
    sent.send_time_ms = 1000;
    sent.size_bytes = 1200;
    controller.OnPacketSent(sent);
    size_t updates = 0;
    controller.SetUpdateCallback([&](const auto&) { ++updates; });
    const auto valid = SignedTwcc(16, {2}, {40});
    auto compound = valid;
    const auto invalid = SignedTwcc(17, {2, 2, 2}, {40});
    compound.insert(compound.end(), invalid.begin(), invalid.end());
    EXPECT_FALSE(controller.OnRtcpPacket(compound.data(), compound.size(), 1100));
    EXPECT_EQ(updates, 0U);
    EXPECT_FALSE(controller.GetNetworkState().HasUpdates());
    ASSERT_TRUE(controller.OnRtcpPacket(valid.data(), valid.size(), 1200));
    EXPECT_EQ(updates, 1U);
    EXPECT_TRUE(controller.GetNetworkState().HasUpdates());
    // The successfully accepted feedback is consumed exactly once.
    ASSERT_TRUE(controller.OnRtcpPacket(valid.data(), valid.size(), 1300));
    EXPECT_EQ(updates, 1U);
}
