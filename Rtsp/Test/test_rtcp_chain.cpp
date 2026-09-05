#include <gtest/gtest.h>

#include "IMediaTransport.h"
#include "MediaEndpoint.h"
#include "GoogCcNetworkController.h"
#include "NackRequester.h"
#include "RtcpNack.h"
#include "RtcpFeedback.h"
#include "RtcpHealper.h"
#include "RtcpReciver.h"
#include "RtpReceiver.h"
#include "RtpSenderTrack.h"
#include "RtpTransportCcExtension.h"
#include "TrackClock.h"
#include "TransportSequenceAllocator.h"
#include "TransportFeedbackGenerator.h"

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

class CaptureTransport final : public IMediaTransport
{
public:
    uint64_t Id() const noexcept override { return 1; }
    MediaTransportProtocol Protocol() const noexcept override
    {
        return MediaTransportProtocol::Udp;
    }
    MediaTransportState State() const noexcept override { return state; }

    SendResult Send(MediaPacketType type,
                    const uint8_t* data,
                    size_t size,
                    bool retransmit) override
    {
        if (!data || size == 0)
        {
            return SendResult::Failed;
        }
        ++send_count;
        last_type = type;
        last_retransmit = retransmit;
        last_packet.assign(data, data + size);
        return SendResult::Ok;
    }

    void Close() override { state = MediaTransportState::Closed; }
    void SetPacketSink(std::weak_ptr<IMediaPacketSink> value) override
    {
        sink = std::move(value);
    }
    int send_count = 0;
    MediaPacketType last_type = MediaPacketType::Rtp;
    bool last_retransmit = false;
    std::vector<uint8_t> last_packet;
    MediaTransportState state = MediaTransportState::Connected;
    std::weak_ptr<IMediaPacketSink> sink;
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

TEST(RtcpChainTest, NackRequesterUsesRttForRetryInterval)
{
    rtsp::NackRequester::Config config;
    config.reorder_wait_ms = 0;
    config.retry_interval_ms = 50;
    rtsp::NackRequester requester(config);

    std::vector<std::vector<uint16_t>> batches;
    requester.SetNackCallback([&batches](const std::vector<uint16_t>& seqs) {
        batches.push_back(seqs);
    });
    requester.UpdateRtt(120);
    requester.OnReceivedPacket(10, 1000);
    requester.OnReceivedPacket(12, 1000);
    requester.Process(1000);
    requester.Process(1119);
    EXPECT_EQ(batches.size(), 1U);
    requester.Process(1120);
    EXPECT_EQ(batches.size(), 2U);
}

TEST(RtcpChainTest, NackRequesterResetsOnLargeSequenceGap)
{
    rtsp::NackRequester::Config config;
    config.reorder_wait_ms = 0;
    config.max_sequence_gap = 100;
    rtsp::NackRequester requester(config);

    std::vector<std::vector<uint16_t>> batches;
    requester.SetNackCallback([&batches](const std::vector<uint16_t>& seqs) {
        batches.push_back(seqs);
    });
    requester.OnReceivedPacket(100, 0);
    requester.OnReceivedPacket(1000, 1);
    requester.Process(1);

    EXPECT_TRUE(batches.empty());
    EXPECT_EQ(requester.GetStats().large_gap_resets, 1U);
    EXPECT_EQ(requester.GetStats().missing_packets, 0U);
}

TEST(RtcpChainTest, NackRequesterReportsRecoveryAndFailure)
{
    rtsp::NackRequester::Config config;
    config.reorder_wait_ms = 0;
    config.retry_interval_ms = 20;
    config.max_retries = 1;
    config.failure_callback_interval_ms = 0;
    rtsp::NackRequester requester(config);

    std::vector<uint16_t> abandoned;
    requester.SetNackCallback([](const std::vector<uint16_t>&) {});
    requester.SetRecoveryFailureCallback(
        [&abandoned](const std::vector<uint16_t>& seqs) { abandoned = seqs; });

    requester.OnReceivedPacket(20, 100);
    requester.OnReceivedPacket(22, 100);
    requester.Process(100);
    const auto recovered = requester.OnReceivedPacket(21, 110);
    EXPECT_TRUE(recovered.was_missing);
    EXPECT_TRUE(recovered.recovered_after_nack);
    EXPECT_EQ(recovered.times_nacked, 1U);
    EXPECT_EQ(requester.GetStats().recovered_packets, 1U);

    requester.OnReceivedPacket(24, 120);
    requester.Process(120);
    requester.Process(140);
    EXPECT_EQ(abandoned, (std::vector<uint16_t>{23}));
    EXPECT_EQ(requester.GetStats().abandoned_packets, 1U);
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

TEST(RtcpChainTest, ReceiveStatsCalculateSrMetricsAndReceiverReport)
{
    RtpRecvStatsBase stats;

    stats.OnRtpPacket(0x11223344, 96, 100, 90000, 1000, 1000, 90000);
    stats.OnRtpPacket(0x11223344, 96, 102, 96000, 1000, 1066, 90000);

    constexpr uint64_t kNtpUnixOffset = 2208988800ULL;
    const uint64_t first_ntp = (kNtpUnixOffset + 10ULL) << 32;
    const uint64_t second_ntp = (kNtpUnixOffset + 15ULL) << 32;
    stats.OnSenderReport(0x11223344, first_ntp, 90000, 100, 100000,
                         1000, 90000);
    stats.OnSenderReport(0x11223344, second_ntp, 540000, 200, 600000,
                         6000, 90000);

    EXPECT_DOUBLE_EQ(stats.GetSrIntervalMs(), 5000.0);
    EXPECT_DOUBLE_EQ(stats.GetSenderBitrateBps(), 800000.0);
    EXPECT_DOUBLE_EQ(stats.GetSenderPacketRate(), 20.0);
    EXPECT_DOUBLE_EQ(stats.GetMeasuredClockRate(), 90000.0);
    EXPECT_DOUBLE_EQ(stats.GetClockDriftPpm(), 0.0);

    const auto report = stats.BuildReceiverReport(7000);
    EXPECT_EQ(report.media_ssrc, 0x11223344U);
    EXPECT_EQ(report.extended_highest_seq, 102U);
    EXPECT_EQ(report.cumulative_lost, 1);
    EXPECT_EQ(report.fraction_lost, 85);
    EXPECT_EQ(report.lsr, static_cast<uint32_t>((second_ntp >> 16) & 0xFFFFFFFFULL));
    EXPECT_EQ(report.dlsr, 65536U);
}

TEST(RtcpChainTest, ReceiveStatsUsesRecentFiveHundredMillisecondBitrateWindow)
{
    RtpRecvStatsBase stats;
    stats.OnRtpPacket(1, 96, 1, 90000, 1000, 1000, 90000);
    stats.OnRtpPacket(1, 96, 2, 112500, 1000, 1250, 90000);

    EXPECT_DOUBLE_EQ(stats.GetReceiverBitrateBps(1250), 64000.0);

    stats.OnRtpPacket(1, 96, 3, 157500, 1000, 1750, 90000);
    EXPECT_DOUBLE_EQ(stats.GetReceiverBitrateBps(1750), 32000.0);
    EXPECT_DOUBLE_EQ(stats.GetReceiverBitrateBps(2251), 0.0);
}

TEST(RtcpChainTest, DispatcherNotifiesQualityPathAfterSenderReport)
{
    TrackInfo info;
    info.type = TrackVideo;
    info.ssrc = 0x11223344;
    info.clock_rate = 90000;
    auto track = std::make_shared<rtsp::RtpVideoTracker>(info);

    rtsp::RtcpDispatcher dispatcher;
    dispatcher.AddReceiverTrack(info.ssrc, track);

    uint32_t notified_ssrc = 0;
    dispatcher.SetSenderReportCallback(
        [&notified_ssrc](uint32_t media_ssrc) {
            notified_ssrc = media_ssrc;
        });

    constexpr uint64_t kNtpUnixOffset = 2208988800ULL;
    dispatcher.OnSenderReport(info.ssrc,
                              (kNtpUnixOffset + 10ULL) << 32,
                              90000, 10, 1000);

    EXPECT_EQ(track->GetSenderReportCount(), 1U);
    EXPECT_EQ(notified_ssrc, info.ssrc);

    dispatcher.OnRttUpdated(info.ssrc, 87);
    EXPECT_EQ(track->GetRttMs(), 87U);
}

TEST(RtcpChainTest, DispatcherRoutesNackAndPliToSenderTrack)
{
    auto transport = std::make_shared<CaptureTransport>();
    rtsp::RtpSenderTrackConfig config;
    config.local_ssrc = 0x11223344;
    auto sender = std::make_shared<rtsp::RtpSenderTrack>(config, transport);

    uint8_t rtp[13] = {0x80, 96, 0, 10, 0, 0, 0, 1,
                       0x11, 0x22, 0x33, 0x44, 0x65};
    ASSERT_TRUE(sender->InputRtpPacket(rtp, sizeof(rtp)));
    ASSERT_EQ(transport->send_count, 1);
    EXPECT_EQ(transport->last_type, MediaPacketType::Rtp);

    int keyframe_requests = 0;
    sender->SetKeyFrameRequestCallback([&keyframe_requests] { ++keyframe_requests; });

    rtsp::RtcpDispatcher dispatcher;
    dispatcher.AddSenderTrack(config.local_ssrc, sender);
    const uint16_t lost[] = {10};
    dispatcher.OnNack(0x55667788, config.local_ssrc, lost, 1);
    dispatcher.OnPli(0x55667788, config.local_ssrc);

    EXPECT_EQ(transport->send_count, 2);
    EXPECT_TRUE(transport->last_retransmit);
    EXPECT_EQ(keyframe_requests, 1);
}

TEST(RtcpChainTest, EndpointBuildsOutboundNack)
{
    TrackInfo info;
    info.type = TrackVideo;
    auto source_track = std::make_shared<RtpTrackDescription>(info);
    auto endpoint = std::make_shared<media::SfuEndpoint>(9, source_track, nullptr, nullptr);
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
    auto source_track = std::make_shared<RtpTrackDescription>(info);
    auto endpoint = std::make_shared<media::SfuEndpoint>(10, source_track, nullptr, nullptr);
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

TEST(RtcpChainTest, ContextFactoriesBuildAndInspectPackets)
{
    auto sender = rtcpx::CreateRtcpSender();
    ASSERT_NE(sender, nullptr);

    std::vector<uint8_t> rr;
    ASSERT_TRUE(sender->GenerateRtcp(0x01020304, rr));
    ASSERT_EQ(rr.size(), 8U);

    rtcpx::RtcpPacketInfo info;
    ASSERT_TRUE(rtcpx::InspectRtcpPacket(rr.data(), rr.size(), &info));
    EXPECT_TRUE(info.valid);
    EXPECT_EQ(info.first_packet_type, rtcpx::RTCP_PT_RR);
    EXPECT_TRUE(info.has_sender_ssrc);
    EXPECT_EQ(info.sender_ssrc, 0x01020304U);
    EXPECT_FALSE(info.has_media_ssrc);

    const auto pli = sender->BuildPli(0x01020304, 0x11223344);
    ASSERT_TRUE(rtcpx::InspectRtcpPacket(pli.data(), pli.size(), &info));
    EXPECT_EQ(info.first_packet_type, rtcpx::RTCP_PT_PSFB);
    EXPECT_EQ(info.sender_ssrc, 0x01020304U);
    EXPECT_EQ(info.media_ssrc, 0x11223344U);

    CaptureObserver observer;
    auto receiver = rtcpx::CreateRtcpReceiver(&observer);
    ASSERT_NE(receiver, nullptr);
    EXPECT_TRUE(receiver->OnRtcpPacket(pli.data(), pli.size()));
}

TEST(RtcpChainTest, SenderBuildsReceiverReportWithReportBlock)
{
    auto sender = rtcpx::CreateRtcpSender();
    ASSERT_NE(sender, nullptr);

    rtcpx::RrBlock block;
    block.ssrc = 0x11223344;
    block.fraction_lost = 64;
    block.cumulative_lost = -2;
    block.extended_highest_seq = 0x00010020;
    block.jitter = 900;
    block.lsr = 0x12345678;
    block.dlsr = 0x00008000;

    const auto packet = sender->BuildReceiverReport(0x01020304, block);
    ASSERT_EQ(packet.size(), 32U);

    CaptureObserver observer;
    rtcpx::RtcpReceiverImpl receiver(&observer);
    ASSERT_TRUE(receiver.OnRtcpPacket(packet.data(), packet.size()));
    ASSERT_EQ(observer.rr_count, 1);
    EXPECT_EQ(observer.reporter, 0x01020304U);
    EXPECT_EQ(observer.media, block.ssrc);
    EXPECT_EQ(observer.fraction, block.fraction_lost);
    EXPECT_EQ(observer.cumulative, block.cumulative_lost);
    EXPECT_EQ(observer.highest, block.extended_highest_seq);
    EXPECT_EQ(observer.last_jitter, block.jitter);
    EXPECT_EQ(observer.last_lsr, block.lsr);
    EXPECT_EQ(observer.last_dlsr, block.dlsr);
}

TEST(RtcpChainTest, GoogCcControllerBoundaryPreservesAvailabilityAndConstraints)
{
    media::NetworkControllerConfig config;
    config.bitrate.min_bitrate_bps = 100000;
    config.bitrate.start_bitrate_bps = 500000;
    config.bitrate.max_bitrate_bps = 1000000;
    media::GoogCcNetworkController controller(config);

    media::WeakNetFeedback feedback;
    feedback.now_ms = 1000;
    feedback.send_bitrate_bps = 500000;
    feedback.receive_bitrate_bps = 500000;
    const auto active = controller.OnReceiverFeedback(feedback);
    EXPECT_TRUE(active.has_target_rate);
    EXPECT_GE(active.target_rate.target_bitrate_bps, config.bitrate.min_bitrate_bps);
    EXPECT_LE(active.target_rate.target_bitrate_bps, config.bitrate.max_bitrate_bps);

    media::NetworkAvailability unavailable;
    unavailable.at_time_ms = 1100;
    unavailable.network_available = false;
    const auto stopped = controller.OnNetworkAvailability(unavailable);
    EXPECT_FALSE(stopped.HasUpdates());

    feedback.now_ms = 1200;
    const auto ignored = controller.OnReceiverFeedback(feedback);
    EXPECT_FALSE(ignored.HasUpdates());
}

TEST(RtcpChainTest, SharedTransportSequenceAllocatorOrdersAudioVideoAndWrapsWireValue)
{
    // 同一 Transport 的音频和视频发送器应共享这个实例。
    auto allocator = std::make_shared<media::TransportSequenceAllocator>(65534);
    auto audio_allocator = allocator;
    auto video_allocator = allocator;

    const auto audio_1 = audio_allocator->Allocate();
    const auto video_1 = video_allocator->Allocate();
    const auto video_2 = video_allocator->Allocate();
    const auto audio_2 = audio_allocator->Allocate();

    EXPECT_EQ(audio_1.wire_sequence, 65534U);
    EXPECT_EQ(video_1.wire_sequence, 65535U);
    EXPECT_EQ(video_2.wire_sequence, 0U);
    EXPECT_EQ(audio_2.wire_sequence, 1U);

    EXPECT_EQ(audio_1.extended_sequence, 65534);
    EXPECT_EQ(video_1.extended_sequence, 65535);
    EXPECT_EQ(video_2.extended_sequence, 65536);
    EXPECT_EQ(audio_2.extended_sequence, 65537);
    EXPECT_EQ(allocator->PeekNextExtendedSequence(), 65538);
}

TEST(RtcpChainTest, SenderTracksShareTwccSequenceAndWriteRtpExtension)
{
    auto allocator = std::make_shared<media::TransportSequenceAllocator>(1000);
    auto audio_transport = std::make_shared<CaptureTransport>();
    auto video_transport = std::make_shared<CaptureTransport>();

    rtsp::RtpSenderTrackConfig audio_config;
    audio_config.local_ssrc = 0x11111111;
    audio_config.transport_cc_extension_id = 3;
    audio_config.transport_sequence_allocator = allocator;
    rtsp::RtpSenderTrack audio(audio_config, audio_transport);

    rtsp::RtpSenderTrackConfig video_config;
    video_config.local_ssrc = 0x22222222;
    video_config.transport_cc_extension_id = 3;
    video_config.transport_sequence_allocator = allocator;
    rtsp::RtpSenderTrack video(video_config, video_transport);

    int64_t audio_extended = -1;
    int64_t video_extended = -1;
    audio.SetPacketSentCallback(
        [&audio_extended](int64_t extended, uint16_t, uint32_t, uint16_t, uint64_t, uint32_t) {
            audio_extended = extended;
        });
    video.SetPacketSentCallback(
        [&video_extended](int64_t extended, uint16_t, uint32_t, uint16_t, uint64_t, uint32_t) {
            video_extended = extended;
        });

    const uint8_t audio_rtp[] = {0x80, 111, 0, 1, 0, 0, 0, 1,
                                 0, 0, 0, 1, 0xAA};
    const uint8_t video_rtp[] = {0x80, 96, 0, 1, 0, 0, 0, 1,
                                 0, 0, 0, 2, 0x65};
    ASSERT_TRUE(audio.InputRtpPacket(audio_rtp, sizeof(audio_rtp)));
    ASSERT_TRUE(video.InputRtpPacket(video_rtp, sizeof(video_rtp)));

    ASSERT_EQ(audio_transport->last_packet.size(), sizeof(audio_rtp) + 8U);
    ASSERT_EQ(video_transport->last_packet.size(), sizeof(video_rtp) + 8U);
    EXPECT_NE(audio_transport->last_packet[0] & 0x10, 0);
    EXPECT_EQ(audio_transport->last_packet[12], 0xBE);
    EXPECT_EQ(audio_transport->last_packet[13], 0xDE);
    EXPECT_EQ(audio_transport->last_packet[16], 0x31); // ID=3, length=2.
    EXPECT_EQ(audio_transport->last_packet[17], 0x03);
    EXPECT_EQ(audio_transport->last_packet[18], 0xE8); // 1000.
    EXPECT_EQ(video_transport->last_packet[17], 0x03);
    EXPECT_EQ(video_transport->last_packet[18], 0xE9); // 1001.
    EXPECT_EQ(audio_extended, 1000);
    EXPECT_EQ(video_extended, 1001);

    uint16_t parsed_audio_twcc = 0;
    uint16_t parsed_video_twcc = 0;
    ASSERT_TRUE(rtsp::RtpTransportCcExtension::Read(
        audio_transport->last_packet.data(), audio_transport->last_packet.size(),
        3, &parsed_audio_twcc));
    ASSERT_TRUE(rtsp::RtpTransportCcExtension::Read(
        video_transport->last_packet.data(), video_transport->last_packet.size(),
        3, &parsed_video_twcc));
    EXPECT_EQ(parsed_audio_twcc, 1000U);
    EXPECT_EQ(parsed_video_twcc, 1001U);
}

TEST(RtcpChainTest, ReceiverTwccGeneratorTracksWrapAndMissingPackets)
{
    rtcpx::TransportFeedbackGenerator::Config config;
    config.feedback_interval_ms = 100;
    config.max_packet_status_count = 100;
    rtcpx::TransportFeedbackGenerator generator(config);

    ASSERT_TRUE(generator.OnPacket(65534, 1000000));
    ASSERT_TRUE(generator.OnPacket(0, 1002000)); // 65535 没有到达。

    rtcpx::TransportFeedbackReport report;
    ASSERT_TRUE(generator.BuildFeedback(0x01020304, 0, 1100000, &report));
    EXPECT_EQ(report.base_sequence, 65534U);
    ASSERT_EQ(report.packet_status_count, 3U);
    ASSERT_EQ(report.packets.size(), 3U);
    EXPECT_TRUE(report.packets[0].received);
    EXPECT_FALSE(report.packets[1].received);
    EXPECT_TRUE(report.packets[2].received);
    EXPECT_EQ(report.packets[0].receive_time_us, 1000000);
    EXPECT_EQ(report.packets[2].receive_time_us, 1002000);
}

TEST(RtcpChainTest, TrackClockMapsRtpTimestampToUnixTime)
{
    constexpr uint64_t kNtpUnixOffset = 2208988800ULL;
    const uint64_t ntp = ((kNtpUnixOffset + 100ULL) << 32) | 0x80000000ULL;

    media::TrackClock clock(90000);
    ASSERT_TRUE(clock.UpdateSenderReport(ntp, 900000));

    const auto mapped = clock.Map(945000);
    ASSERT_TRUE(mapped.valid);
    EXPECT_EQ(mapped.unix_time_us, 101000000);
}

TEST(RtcpChainTest, TrackClockHandlesRtpTimestampWrap)
{
    constexpr uint64_t kNtpUnixOffset = 2208988800ULL;
    const uint64_t ntp = (kNtpUnixOffset + 10ULL) << 32;

    media::TrackClock clock(90000);
    ASSERT_TRUE(clock.UpdateSenderReport(ntp, 0xFFFFFF00U));

    const auto mapped = clock.Map(0x00000100U);
    ASSERT_TRUE(mapped.valid);
    EXPECT_EQ(mapped.unix_time_us, 10005688);
}

TEST(RtcpChainTest, H264SingleNaluProducesCompleteEncodedFrame)
{
    TrackInfo info;
    info.type = TrackVideo;
    info.codec_id = CodecId::H264;
    info.clock_rate = 90000;
    info.track_index = 3;

    rtsp::RtpVideoTracker tracker(info);
    media::EncodedFrame::Ptr output;
    tracker.setOnEncodedFrame([&output](const media::EncodedFrame::Ptr& frame) {
        output = frame;
    });

    uint8_t rtp[] = {
        0x80, 0xE0,             // V=2, marker=1, PT=96.
        0x00, 0x10,             // Sequence number.
        0x00, 0x01, 0x5F, 0x90, // RTP timestamp 90000.
        0x11, 0x22, 0x33, 0x44, // SSRC.
        0x65, 0xAA, 0xBB        // IDR NALU.
    };

    ASSERT_NE(tracker.inputRtp(rtp, sizeof(rtp)), nullptr);
    ASSERT_NE(output, nullptr);
    EXPECT_TRUE(output->Valid());
    EXPECT_TRUE(output->IsComplete());
    EXPECT_TRUE(output->IsKeyFrame());
    EXPECT_TRUE(output->video.is_idr);
    EXPECT_FALSE(output->video.parameter_sets_injected);
    EXPECT_EQ(output->info.track_id, 3U);
    EXPECT_EQ(output->rtp.rtp_timestamp, 90000U);
    EXPECT_GT(output->info.timestamp.receive_time_ms, 0);
    EXPECT_FALSE(output->info.timestamp.capture_time_valid);
    EXPECT_EQ(*output->buffer,
              (std::vector<uint8_t>{0, 0, 0, 1, 0x65, 0xAA, 0xBB}));
}

TEST(RtcpChainTest, ConsecutiveSortedVideoPacketsRemainComplete)
{
    TrackInfo info;
    info.type = TrackVideo;
    info.codec_id = CodecId::H264;
    info.clock_rate = 90000;

    rtsp::RtpVideoTracker tracker(info);
    std::vector<media::EncodedFrame::Ptr> outputs;
    tracker.setOnEncodedFrame([&outputs](const media::EncodedFrame::Ptr& frame) {
        outputs.push_back(frame);
    });

    uint8_t first[] = {
        0x80, 0xE0, 0x12, 0x71,
        0x00, 0x01, 0x5F, 0x90,
        0x11, 0x22, 0x33, 0x44,
        0x65, 0xAA
    };
    uint8_t second[] = {
        0x80, 0xE0, 0x12, 0x72,
        0x00, 0x02, 0xBF, 0x20,
        0x11, 0x22, 0x33, 0x44,
        0x41, 0xBB
    };

    ASSERT_NE(tracker.inputRtp(first, sizeof(first)), nullptr);
    ASSERT_NE(tracker.inputRtp(second, sizeof(second)), nullptr);
    ASSERT_EQ(outputs.size(), 2U);
    EXPECT_TRUE(outputs[0]->IsComplete());
    EXPECT_TRUE(outputs[1]->IsComplete());
}

TEST(RtcpChainTest, H264FrameWaitsForRetransmittedGap)
{
    TrackInfo info;
    info.type = TrackVideo;
    info.codec_id = CodecId::H264;
    info.clock_rate = 90000;

    rtsp::RtpVideoTracker tracker(info);
    std::vector<media::EncodedFrame::Ptr> outputs;
    tracker.setOnEncodedFrame([&outputs](const media::EncodedFrame::Ptr& frame) {
        outputs.push_back(frame);
    });

    uint8_t start[] = {0x80, 0x60, 0x00, 0x64, 0, 0, 0, 1,
                       1, 2, 3, 4, 0x7C, 0x85, 0xAA};
    uint8_t middle[] = {0x80, 0x60, 0x00, 0x65, 0, 0, 0, 1,
                        1, 2, 3, 4, 0x7C, 0x05, 0xBB};
    uint8_t end[] = {0x80, 0xE0, 0x00, 0x66, 0, 0, 0, 1,
                     1, 2, 3, 4, 0x7C, 0x45, 0xCC};

    ASSERT_NE(tracker.inputRtp(start, sizeof(start)), nullptr);
    ASSERT_NE(tracker.inputRtp(end, sizeof(end)), nullptr);
    EXPECT_TRUE(outputs.empty());

    ASSERT_NE(tracker.inputRtp(middle, sizeof(middle)), nullptr);
    ASSERT_EQ(outputs.size(), 1U);
    EXPECT_EQ(*outputs.front()->buffer,
              (std::vector<uint8_t>{0, 0, 0, 1, 0x65, 0xAA, 0xBB, 0xCC}));
    EXPECT_EQ(outputs.front()->rtp.first_sequence, 100U);
    EXPECT_EQ(outputs.front()->rtp.last_sequence, 102U);
}

TEST(RtcpChainTest, H264PacketBufferHandlesSequenceWrap)
{
    TrackInfo info;
    info.type = TrackVideo;
    info.codec_id = CodecId::H264;
    info.clock_rate = 90000;

    rtsp::RtpVideoTracker tracker(info);
    media::EncodedFrame::Ptr output;
    tracker.setOnEncodedFrame([&output](const media::EncodedFrame::Ptr& frame) {
        output = frame;
    });

    uint8_t start[] = {0x80, 0x60, 0xFF, 0xFF, 0, 0, 0, 2,
                       1, 2, 3, 4, 0x7C, 0x85, 0x11};
    uint8_t end[] = {0x80, 0xE0, 0x00, 0x00, 0, 0, 0, 2,
                     1, 2, 3, 4, 0x7C, 0x45, 0x22};

    ASSERT_NE(tracker.inputRtp(start, sizeof(start)), nullptr);
    ASSERT_NE(tracker.inputRtp(end, sizeof(end)), nullptr);
    ASSERT_NE(output, nullptr);
    EXPECT_EQ(output->rtp.first_sequence, 65535U);
    EXPECT_EQ(output->rtp.last_sequence, 0U);
    EXPECT_EQ(*output->buffer,
              (std::vector<uint8_t>{0, 0, 0, 1, 0x65, 0x11, 0x22}));
}

TEST(RtcpChainTest, H264PacketBufferReplacesOlderRingWindow)
{
    TrackInfo info;
    info.type = TrackVideo;
    info.codec_id = CodecId::H264;
    info.clock_rate = 90000;

    rtsp::RtpVideoTracker tracker(info);
    media::EncodedFrame::Ptr output;
    tracker.setOnEncodedFrame([&output](const media::EncodedFrame::Ptr& frame) {
        output = frame;
    });

    uint8_t old_start[] = {0x80, 0x60, 0x00, 0x00, 0, 0, 0, 1,
                           1, 2, 3, 4, 0x7C, 0x85, 0xAA};
    uint8_t new_idr[] = {0x80, 0xE0, 0x08, 0x00, 0, 0, 0, 2,
                         1, 2, 3, 4, 0x65, 0xBB};

    ASSERT_NE(tracker.inputRtp(old_start, sizeof(old_start)), nullptr);
    EXPECT_EQ(output, nullptr);
    ASSERT_NE(tracker.inputRtp(new_idr, sizeof(new_idr)), nullptr);
    ASSERT_NE(output, nullptr);
    EXPECT_EQ(output->rtp.first_sequence, 2048U);
    EXPECT_EQ(output->rtp.last_sequence, 2048U);
}
