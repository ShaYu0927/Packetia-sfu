#include <gtest/gtest.h>
#include "KeyFrameRequestLimiter.h"
#include "NackRequester.h"
#include "RtcpNack.h"
#include "RtpSenderTrack.h"

namespace
{
class LossRecovery : public ::testing::Test
{
protected:
    rtsp::NackRequester::Config Config()
    {
        rtsp::NackRequester::Config config;
        config.reorder_wait_ms = 20;
        config.max_retries = 1;
        return config;
    }
    void Observe(rtsp::NackRequester& receiver)
    {
        receiver.SetNackCallback([this](const auto& batch) { nacks.push_back(batch); });
        receiver.SetRecoveryFailureCallback([this](const auto& batch) { failures.push_back(batch); });
    }
    void Gap(rtsp::NackRequester& receiver, uint64_t at_ms = 0)
    {
        receiver.OnReceivedPacket(100, at_ms);
        receiver.OnReceivedPacket(102, at_ms);
    }
    std::vector<std::vector<uint16_t>> nacks, failures;
};

class CaptureTransport final : public IMediaTransport
{
public:
    uint64_t Id() const noexcept override { return 1; }
    MediaTransportProtocol Protocol() const noexcept override { return MediaTransportProtocol::Udp; }
    MediaTransportState State() const noexcept override { return MediaTransportState::Connected; }
    SendResult Send(MediaPacketType, const uint8_t* data, size_t size, bool retry) override
    {
        if (fail) return SendResult::Failed;
        packets.emplace_back(data, data + size);
        retransmissions += retry ? 1 : 0;
        return SendResult::Ok;
    }
    void Close() override {}
    void SetPacketSink(std::weak_ptr<IMediaPacketSink>) override {}
    bool fail = false;
    size_t retransmissions = 0;
    std::vector<std::vector<uint8_t>> packets;
};

std::vector<uint8_t> Rtp(uint16_t seq)
{
    return {0x80, 96, static_cast<uint8_t>(seq >> 8), static_cast<uint8_t>(seq),
        0, 0, 0, 1, 0, 0, 0, 33, 0x65, 0xaa};
}
}

TEST_F(LossRecovery, FinalRequestWaitsForResponseInsteadOfFailingOnNextTick)
{
    rtsp::NackRequester receiver(Config());
    Observe(receiver);
    receiver.UpdateRtt(120);
    Gap(receiver);
    receiver.Process(19);
    EXPECT_TRUE(nacks.empty());
    receiver.Process(20);
    ASSERT_EQ(nacks.size(), 1U);
    receiver.Process(21);
    receiver.Process(139);
    EXPECT_TRUE(failures.empty());
    const auto result = receiver.OnReceivedPacket(101, 140);
    EXPECT_TRUE(result.recovered_after_nack);
    receiver.Process(500);
    EXPECT_TRUE(failures.empty());
    EXPECT_EQ(receiver.GetStats().recovered_packets, 1U);
}

TEST_F(LossRecovery, ExhaustedRequestEventuallyReportsFailureWithoutNewRtp)
{
    rtsp::NackRequester receiver(Config());
    Observe(receiver);
    receiver.UpdateRtt(120);
    Gap(receiver);
    receiver.Process(20);
    receiver.Process(139);
    EXPECT_TRUE(failures.empty());
    receiver.Process(140);
    ASSERT_EQ(failures.size(), 1U);
    EXPECT_EQ(failures.front(), (std::vector<uint16_t>{101}));
    receiver.Process(1000);
    EXPECT_EQ(nacks.size(), 1U);
    EXPECT_EQ(failures.size(), 1U);
}

TEST_F(LossRecovery, HighRttIsNotClippedToFallbackMaximum)
{
    auto config = Config();
    config.max_retries = 3;
    config.max_packet_age_ms = 2000;
    rtsp::NackRequester receiver(config);
    Observe(receiver);
    receiver.UpdateRtt(350);
    Gap(receiver);
    receiver.Process(20);
    receiver.Process(220);
    receiver.Process(369);
    EXPECT_EQ(nacks.size(), 1U);
    receiver.Process(370);
    EXPECT_EQ(nacks.size(), 2U);
}

TEST_F(LossRecovery, DoesNotRequestRepairThatCannotFitInRecoveryBudget)
{
    rtsp::NackRequester receiver(Config());
    Observe(receiver);
    receiver.UpdateRtt(500);
    Gap(receiver);
    receiver.Process(20);
    EXPECT_TRUE(nacks.empty());
    ASSERT_EQ(failures.size(), 1U);
    EXPECT_EQ(receiver.GetStats().abandoned_packets, 1U);
}

TEST_F(LossRecovery, DeadlineStillAppliesWhileWaitingForFinalResponse)
{
    rtsp::NackRequester receiver(Config());
    Observe(receiver);
    receiver.UpdateRtt(100);
    Gap(receiver);
    receiver.Process(399); // Last request still has 101 ms left.
    ASSERT_EQ(nacks.size(), 1U);
    receiver.UpdateRtt(300);
    receiver.Process(500);
    EXPECT_EQ(failures.size(), 1U);
}

TEST_F(LossRecovery, FullSendBatchDoesNotPreventExpiredEntryCleanup)
{
    auto config = Config();
    config.max_batch_size = 1;
    config.max_retries = 3;
    config.reorder_wait_ms = 0;
    config.max_sequence_gap = 1000;
    rtsp::NackRequester receiver(config);
    Observe(receiver);
    receiver.OnReceivedPacket(65533, 0);
    receiver.OnReceivedPacket(65535, 0); // Old missing 65534 sorts after missing 0.
    receiver.OnReceivedPacket(1, 490);
    receiver.Process(500);
    ASSERT_EQ(nacks.size(), 1U);
    EXPECT_EQ(nacks[0], (std::vector<uint16_t>{0}));
    ASSERT_EQ(failures.size(), 1U);
    EXPECT_EQ(failures[0], (std::vector<uint16_t>{65534}));
}

TEST_F(LossRecovery, ReorderedAndWrappedPacketsDoNotNeedRetransmission)
{
    rtsp::NackRequester receiver(Config());
    Observe(receiver);
    receiver.OnReceivedPacket(65535, 0);
    receiver.OnReceivedPacket(1, 0);
    const auto result = receiver.OnReceivedPacket(0, 19);
    EXPECT_TRUE(result.was_missing);
    EXPECT_FALSE(result.recovered_after_nack);
    receiver.Process(20);
    EXPECT_TRUE(nacks.empty());
    EXPECT_EQ(receiver.GetStats().reordered_packets, 1U);
}

TEST_F(LossRecovery, MissingCallbackDoesNotConsumeRetryAttempts)
{
    rtsp::NackRequester receiver(Config());
    Gap(receiver);
    receiver.Process(20);
    EXPECT_EQ(receiver.GetStats().nack_requests, 0U);
    Observe(receiver);
    receiver.Process(21);
    EXPECT_EQ(nacks.size(), 1U);
}

TEST_F(LossRecovery, FailureCallbackAtTimeZeroIsStillRateLimitedAndResettable)
{
    auto config = Config();
    config.max_retries = 0;
    rtsp::NackRequester receiver(config);
    Observe(receiver);
    Gap(receiver);
    receiver.Process(0);
    receiver.OnReceivedPacket(104, 1);
    receiver.Process(1);
    EXPECT_EQ(failures.size(), 1U);
    receiver.Reset();
    Gap(receiver);
    receiver.Process(0);
    EXPECT_EQ(failures.size(), 2U);
}

TEST(LossRecoverySender, OriginalSendAgeLimitsRetriesEvenWhenCacheIsNotFull)
{
    auto transport = std::make_shared<CaptureTransport>();
    rtsp::RtpSenderTrackConfig config;
    config.local_ssrc = 33;
    uint64_t now = 0;
    rtsp::RtpSenderTrack sender(config, transport, [&] { return now; });
    const auto packet = Rtp(101);
    ASSERT_TRUE(sender.InputRtpPacket(packet.data(), packet.size()));
    now = 400;
    sender.OnRtcpNack({101});
    ASSERT_EQ(transport->retransmissions, 1U);
    now = 500;
    sender.OnRtcpNack({101});
    EXPECT_EQ(transport->retransmissions, 1U); // Retry at 400 did not renew the age.
}

TEST(LossRecoverySender, DuplicateNacksAtTimeZeroRespectIntervalAndAttemptLimit)
{
    auto transport = std::make_shared<CaptureTransport>();
    rtsp::RtpSenderTrackConfig config;
    config.local_ssrc = 33;
    config.max_retransmit_count = 2;
    uint64_t now = 0;
    rtsp::RtpSenderTrack sender(config, transport, [&] { return now; });
    const auto packet = Rtp(101);
    ASSERT_TRUE(sender.InputRtpPacket(packet.data(), packet.size()));
    sender.OnRtcpNack({101, 101, 101});
    EXPECT_EQ(transport->retransmissions, 1U);
    now = 30;
    sender.OnRtcpNack({101});
    now = 60;
    sender.OnRtcpNack({101});
    EXPECT_EQ(transport->retransmissions, 2U);
}

TEST(LossRecoverySender, FailedRetryDoesNotConsumeAnAttempt)
{
    auto transport = std::make_shared<CaptureTransport>();
    rtsp::RtpSenderTrackConfig config;
    config.local_ssrc = 33;
    config.max_retransmit_count = 1;
    rtsp::RtpSenderTrack sender(config, transport, [] { return 100; });
    const auto packet = Rtp(101);
    ASSERT_TRUE(sender.InputRtpPacket(packet.data(), packet.size()));
    transport->fail = true;
    sender.OnRtcpNack({101});
    transport->fail = false;
    sender.OnRtcpNack({101});
    EXPECT_EQ(transport->retransmissions, 1U);
}

TEST_F(LossRecovery, WireNackRetransmitsOriginalSequenceAndRecoversBeforeDeadline)
{
    auto transport = std::make_shared<CaptureTransport>();
    rtsp::RtpSenderTrackConfig config;
    config.local_ssrc = 33;
    uint64_t now = 0;
    rtsp::RtpSenderTrack sender(config, transport, [&] { return now; });
    for (uint16_t seq : {100, 101, 102})
    {
        const auto packet = Rtp(seq);
        ASSERT_TRUE(sender.InputRtpPacket(packet.data(), packet.size()));
    }
    rtsp::NackRequester receiver(Config());
    Observe(receiver);
    receiver.UpdateRtt(80);
    receiver.SetNackCallback([&](const auto& seqs) {
        const auto wire = rtcpx::RtRtcpNack::Build(44, 33, seqs);
        uint32_t reporter = 0, source = 0;
        std::vector<uint16_t> lost;
        ASSERT_TRUE(rtcpx::RtRtcpNack::Parse(wire.data(), wire.size(), &reporter, &source, &lost));
        EXPECT_EQ(source, 33U);
        sender.OnRtcpNack(lost);
    });
    Gap(receiver);
    now = 20;
    receiver.Process(now);
    ASSERT_EQ(transport->retransmissions, 1U);
    receiver.Process(21);
    EXPECT_TRUE(failures.empty());
    const auto& repaired = transport->packets.back();
    const auto seq = static_cast<uint16_t>((repaired[2] << 8) | repaired[3]);
    EXPECT_EQ(seq, 101);
    EXPECT_TRUE(receiver.OnReceivedPacket(seq, 100).recovered_after_nack);
    receiver.Process(500);
    EXPECT_TRUE(failures.empty());
}

TEST(KeyFrameRequestLimiterTest, RequestsShareOneBudgetAndZeroIsAValidTime)
{
    media::KeyFrameRequestLimiter source;
    EXPECT_TRUE(source.Allow(0));
    for (int subscriber = 0; subscriber < 100; ++subscriber)
        EXPECT_FALSE(source.Allow(1));
    EXPECT_FALSE(source.Allow(999));
    EXPECT_TRUE(source.Allow(1000));
    media::KeyFrameRequestLimiter other_source;
    EXPECT_TRUE(other_source.Allow(1));
}
