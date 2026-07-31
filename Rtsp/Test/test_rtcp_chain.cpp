#include <gtest/gtest.h>

#include "IMediaTransport.h"
#include "MediaEndpoint.h"
#include "NackRequester.h"
#include "RtcpNack.h"
#include "RtcpFeedback.h"
#include "RtcpReciver.h"
#include "RtpReceiver.h"
#include "RtpSenderTrack.h"

namespace
{
class CaptureObserver final : public rtcpx::IRtcpObserver
{
public:
    void OnReceiverReport(uint32_t sender_ssrc,
                          uint32_t media_ssrc,
                          uint8_t fraction_lost,
                          int32_t cumulative_lost,
                          uint32_t highest_seq,
                          uint32_t jitter,
                          uint32_t lsr,
                          uint32_t dlsr) override
    {
        ++rr_count;
        reporter = sender_ssrc;
        media = media_ssrc;
        fraction = fraction_lost;
        cumulative = cumulative_lost;
        highest = highest_seq;
        last_jitter = jitter;
        last_lsr = lsr;
        last_dlsr = dlsr;
    }

    void OnSenderReport(uint32_t, uint64_t, uint32_t, uint32_t, uint32_t) override {}
    void OnNack(uint32_t, uint32_t, const uint16_t*, size_t) override {}
    void OnPli(uint32_t, uint32_t) override {}
    void OnFir(uint32_t, uint32_t, uint8_t) override {}
    void OnBye(uint32_t) override {}
    void OnRttUpdated(uint32_t, uint32_t) override {}

    int rr_count = 0;
    uint32_t reporter = 0;
    uint32_t media = 0;
    uint8_t fraction = 0;
    int32_t cumulative = 0;
    uint32_t highest = 0;
    uint32_t last_jitter = 0;
    uint32_t last_lsr = 0;
    uint32_t last_dlsr = 0;
};

class CaptureSender final : public IPacketSender
{
public:
    SendResult SendPacket(const uint8_t* data, size_t size, const SendOptions& options) override
    {
        if (!data || size == 0)
        {
            return SendResult::Failed;
        }
        ++send_count;
        last_options = options;
        return SendResult::Ok;
    }

    int send_count = 0;
    SendOptions last_options;
};

void WriteU32(uint8_t* p, uint32_t value)
{
    p[0] = static_cast<uint8_t>(value >> 24);
    p[1] = static_cast<uint8_t>(value >> 16);
    p[2] = static_cast<uint8_t>(value >> 8);
    p[3] = static_cast<uint8_t>(value);
}
}

TEST(RtcpChainTest, NackBuildParseRoundTrip)
{
    const std::vector<uint16_t> lost{100, 101, 103, 120};
    const auto packet = rtcpx::RtRtcpNack::Build(0x01020304, 0x11223344, lost);

    uint32_t sender_ssrc = 0;
    uint32_t media_ssrc = 0;
    std::vector<uint16_t> parsed;
    ASSERT_TRUE(rtcpx::RtRtcpNack::Parse(packet.data(), packet.size(),
                                         &sender_ssrc, &media_ssrc, &parsed));
    EXPECT_EQ(sender_ssrc, 0x01020304U);
    EXPECT_EQ(media_ssrc, 0x11223344U);
    EXPECT_EQ(parsed, lost);
}

TEST(RtcpChainTest, NackRequesterEmitsAfterReorderWindow)
{
    rtsp::NackRequester::Config config;
    config.reorder_wait_ms = 20;
    config.retry_interval_ms = 50;
    rtsp::NackRequester requester(config);

    std::vector<std::vector<uint16_t>> batches;
    requester.SetNackCallback([&batches](const std::vector<uint16_t>& seqs) {
        batches.push_back(seqs);
    });

    requester.OnReceivedPacket(100, 0);
    requester.OnReceivedPacket(102, 0);
    requester.Process(19);
    EXPECT_TRUE(batches.empty());

    requester.Process(20);
    ASSERT_EQ(batches.size(), 1U);
    EXPECT_EQ(batches[0], (std::vector<uint16_t>{101}));

    requester.Process(69);
    EXPECT_EQ(batches.size(), 1U);
    requester.Process(70);
    ASSERT_EQ(batches.size(), 2U);
    EXPECT_EQ(batches[1], (std::vector<uint16_t>{101}));
}

TEST(RtcpChainTest, ReceiverReportUsesSigned24BitCumulativeLoss)
{
    uint8_t rr[32] = {};
    rr[0] = 0x81; // V=2, one report block.
    rr[1] = 201;
    rr[2] = 0;
    rr[3] = 7;
    WriteU32(rr + 4, 0x01020304);
    WriteU32(rr + 8, 0x11223344);
    rr[12] = 64;
    rr[13] = 0xFF;
    rr[14] = 0xFF;
    rr[15] = 0xFE; // signed 24-bit -2.
    WriteU32(rr + 16, 0x00010020);
    WriteU32(rr + 20, 900);
    WriteU32(rr + 24, 0x12345678);
    WriteU32(rr + 28, 0x00008000);

    CaptureObserver observer;
    rtcpx::RtcpReceiverImpl receiver(&observer);
    ASSERT_TRUE(receiver.OnRtcpPacket(rr, sizeof(rr)));
    ASSERT_EQ(observer.rr_count, 1);
    EXPECT_EQ(observer.reporter, 0x01020304U);
    EXPECT_EQ(observer.media, 0x11223344U);
    EXPECT_EQ(observer.fraction, 64);
    EXPECT_EQ(observer.cumulative, -2);
    EXPECT_EQ(observer.highest, 0x00010020U);
}

TEST(RtcpChainTest, DispatcherRoutesNackAndPliToSenderTrack)
{
    CaptureSender transport;
    rtsp::RtpSenderTrackConfig config;
    config.local_ssrc = 0x11223344;
    auto sender = std::make_shared<rtsp::RtpSenderTrack>(config, &transport);

    uint8_t rtp[13] = {0x80, 96, 0, 10, 0, 0, 0, 1,
                       0x11, 0x22, 0x33, 0x44, 0x65};
    ASSERT_TRUE(sender->InputRtpPacket(rtp, sizeof(rtp)));
    ASSERT_EQ(transport.send_count, 1);

    int keyframe_requests = 0;
    sender->SetKeyFrameRequestCallback([&keyframe_requests] { ++keyframe_requests; });

    rtsp::RtcpDispatcher dispatcher;
    dispatcher.AddSenderTrack(config.local_ssrc, sender);
    const uint16_t lost[] = {10};
    dispatcher.OnNack(0x55667788, config.local_ssrc, lost, 1);
    dispatcher.OnPli(0x55667788, config.local_ssrc);

    EXPECT_EQ(transport.send_count, 2);
    EXPECT_TRUE(transport.last_options.retransmit);
    EXPECT_EQ(keyframe_requests, 1);
}

TEST(RtcpChainTest, EndpointBuildsOutboundNack)
{
    TrackInfo info;
    info.type = TrackVideo;
    auto source_track = std::make_shared<VideoTrack>(info);
    auto endpoint = std::make_shared<media::SfuEndpoint>(9, source_track, nullptr);
    ASSERT_TRUE(endpoint->Start());

    std::vector<uint8_t> sent;
    endpoint->SetRtcpSendCallback([&sent](const uint8_t* data, size_t len) {
        sent.assign(data, data + len);
        return true;
    });
    endpoint->OnTrackNack(0x11223344, {10, 11, 14});

    uint32_t sender_ssrc = 0;
    uint32_t media_ssrc = 0;
    std::vector<uint16_t> lost;
    ASSERT_TRUE(rtcpx::RtRtcpNack::Parse(sent.data(), sent.size(),
                                         &sender_ssrc, &media_ssrc, &lost));
    EXPECT_EQ(sender_ssrc, 9U);
    EXPECT_EQ(media_ssrc, 0x11223344U);
    EXPECT_EQ(lost, (std::vector<uint16_t>{10, 11, 14}));
}

TEST(RtcpChainTest, EndpointBuildsOutboundPli)
{
    TrackInfo info;
    info.type = TrackVideo;
    auto source_track = std::make_shared<VideoTrack>(info);
    auto endpoint = std::make_shared<media::SfuEndpoint>(10, source_track, nullptr);
    ASSERT_TRUE(endpoint->Start());

    std::vector<uint8_t> sent;
    endpoint->SetRtcpSendCallback([&sent](const uint8_t* data, size_t len) {
        sent.assign(data, data + len);
        return true;
    });
    endpoint->OnTrackPli(0x11223344);

    uint32_t sender_ssrc = 0;
    uint32_t media_ssrc = 0;
    ASSERT_TRUE(rtcpx::RtcpFeedback::ParsePli(sent.data(), sent.size(),
                                              &sender_ssrc, &media_ssrc));
    EXPECT_EQ(sender_ssrc, 10U);
    EXPECT_EQ(media_ssrc, 0x11223344U);
}
