#include <gtest/gtest.h>

#include "DownlinkTrackManager.h"
#include "ReceiveFeedbackHandler.h"
#include "SfuEndpoint.h"
#include "MediaStreamAffinity.h"

namespace
{
class TestTransport final : public IMediaTransport
{
public:
    uint64_t Id() const noexcept override { return 1; }
    MediaTransportProtocol Protocol() const noexcept override { return MediaTransportProtocol::Udp; }
    MediaTransportState State() const noexcept override { return MediaTransportState::Connected; }
    SendResult Send(MediaPacketType, const uint8_t*, size_t, bool) override { return SendResult::Ok; }
    void SetPacketSink(std::weak_ptr<IMediaPacketSink>) override {}
    void Close() override { ++closed; }
    unsigned closed = 0;
};

TEST(EndpointComponents, DownlinkOwnerClosesRetainedRoutesAndRemovesFeedbackRegistration)
{
    auto source = std::make_shared<media::SfuEndpoint>(1);
    auto destination = std::make_shared<media::SfuEndpoint>(2);
    auto transport = std::make_shared<TestTransport>();
    auto retained = std::make_shared<media::endpoint::Subscription>();
    retained->source = source;
    retained->destination = destination;
    retained->source_track_id = "camera";
    rtsp::RtcpDispatcher dispatcher;
    {
        media::endpoint::DownlinkTrackManager downlinks;
        downlinks.SetDispatcher(&dispatcher);
        rtsp::RtpSenderTrackConfig config;
        config.local_ssrc = 777;
        ASSERT_TRUE(downlinks.Configure("camera", config, transport, CodecId::H264));
        ::TrackInfo info;
        info.codec_id = CodecId::H264;
        info.clock_rate = 90000;
        ASSERT_TRUE(downlinks.Subscribe("camera", info, {}, retained, {}));
        EXPECT_EQ(downlinks.Count(), 1U);
        dispatcher.OnReceiverReport(99, 777, 0, 0, 1, 0, 0, 0);
        EXPECT_EQ(retained->sender->GetReceiverReportCount(), 1U);
    }
    // 即使排队任务仍持有关系，管理器析构也先关闸门并撤销 RTCP 索引。
    EXPECT_TRUE(retained->Closed());
    dispatcher.OnReceiverReport(99, 777, 0, 0, 2, 0, 0, 0);
    EXPECT_EQ(retained->sender->GetReceiverReportCount(), 1U);
    EXPECT_EQ(transport->closed, 0U);
}

TEST(EndpointComponents, ReceiveFeedbackReportsTheReceivedSourceWithTheLocalRtcpIdentity)
{
    ::TrackInfo info;
    info.type = TrackAudio;
    info.codec_id = CodecId::OPUS;
    info.codec_name = "opus";
    info.payload_type = 111;
    info.clock_rate = 48000;
    info.channels = 2;
    info.ssrc = 77;
    const auto track = rtsp::RtpReceiverTrack::Create(info);
    ASSERT_TRUE(track);
    const std::vector<uint8_t> packet{0x80, 111, 0, 1, 0, 0, 0, 1,
        0, 0, 0, 77, 0xf8, 0xff, 0xfe};
    ASSERT_TRUE(track->inputRtp(packet.data(), packet.size()));
    media::endpoint::ReceiveFeedbackHandler feedback;
    ASSERT_TRUE(feedback.RegisterSource(info.ssrc));
    std::vector<uint8_t> report;
    const auto send = [&](const uint8_t* data, size_t size) {
        report.assign(data, data + size);
        return true;
    };
    EXPECT_FALSE(feedback.Evaluate(track, 5000, 99, send));
    ASSERT_GE(report.size(), 32U);
    EXPECT_EQ(report[1], 201U);
    EXPECT_EQ(media_affinity::ReadUint32BE(report.data() + 4), 99U);
    EXPECT_EQ(media_affinity::ReadUint32BE(report.data() + 8), 77U);
    feedback.Remove(info.ssrc);
    report.clear();
    EXPECT_FALSE(feedback.Evaluate(track, 6000, 99, send));
    EXPECT_TRUE(report.empty());
    EXPECT_FALSE(feedback.GetSnapshot(info.ssrc));
}

TEST(EndpointComponents, ReceiverCreatesQualityBeforeFirstSrAndRemovesItWithTheTrack)
{
    auto endpoint = std::make_shared<media::SfuEndpoint>(901);
    ASSERT_TRUE(endpoint->Start());
    for (uint32_t ssrc : {77U, 88U})
    {
        ::TrackInfo info;
        info.track_index = ssrc == 77 ? 0 : 1;
        info.type = TrackAudio; info.codec_id = CodecId::OPUS;
        info.codec_name = "opus"; info.payload_type = 111;
        info.clock_rate = 48000; info.channels = 2; info.ssrc = ssrc;
        ASSERT_TRUE(endpoint->AddPublishedTrack(std::to_string(ssrc), std::make_shared<RtpTrackDescription>(info)));
        EXPECT_FALSE(endpoint->GetReceiveQualitySnapshot(ssrc));
    }
    unsigned reports = 0;
    endpoint->SetRtcpSendCallback([&](const uint8_t*, size_t) { ++reports; return true; });
    std::vector<uint8_t> sr(28, 0);
    sr[0] = 0x80; sr[1] = 200; sr[3] = 6; sr[7] = 77;
    sr[8] = 0xe9; sr[12] = 1;
    WorkJob feedback;
    feedback.payload = common::SharedBuffer::TryCopy(sr.data(), sr.size());
    for (uint8_t ssrc : {77, 88})
    {
        const std::vector<uint8_t> packet{0x80, 111, 0, 1, 0, 0, 0, 1,
            0, 0, 0, ssrc, 0xf8, 0xff, 0xfe};
        WorkJob job;
        job.payload = common::SharedBuffer::TryCopy(packet.data(), packet.size());
        endpoint->OnRtp(job);
        const auto initial = endpoint->GetReceiveQualitySnapshot(ssrc);
        ASSERT_TRUE(initial);
        EXPECT_EQ(initial->quality, media::NetworkQualityLevel::Unknown);
        EXPECT_EQ(initial->update_time_ms, 0U);
        EXPECT_FALSE(initial->smoothed.loss_rate);
    }
    EXPECT_EQ(reports, 0U); // Registration does not sample or send RTCP.
    endpoint->OnRtcp(feedback);
    const auto evaluated = endpoint->GetReceiveQualitySnapshot(77);
    ASSERT_TRUE(evaluated);
    EXPECT_GT(evaluated->update_time_ms, 0U);
    EXPECT_EQ(evaluated->quality, media::NetworkQualityLevel::Excellent);
    EXPECT_TRUE(evaluated->smoothed.loss_rate);
    EXPECT_EQ(reports, 1U);
    EXPECT_EQ(endpoint->GetReceiveQualitySnapshot(88)->quality, media::NetworkQualityLevel::Unknown);
    ASSERT_TRUE(endpoint->RemovePublishedTrack("77"));
    EXPECT_FALSE(endpoint->GetReceiveQualitySnapshot(77));
    EXPECT_TRUE(endpoint->GetReceiveQualitySnapshot(88));
    endpoint->OnRtcp(feedback); // A delayed SR cannot recreate the removed evaluator.
    EXPECT_FALSE(endpoint->GetReceiveQualitySnapshot(77));
    EXPECT_EQ(reports, 1U);
    endpoint->Stop();
    EXPECT_FALSE(endpoint->GetReceiveQualitySnapshot(88));
}

TEST(EndpointComponents, FailedReceiverCreationDoesNotLeaveQualityState)
{
    auto endpoint = std::make_shared<media::SfuEndpoint>(902);
    ::TrackInfo info;
    info.track_index = 0;
    info.type = TrackAudio; info.codec_id = CodecId::Unknown;
    info.payload_type = 111; info.clock_rate = 48000; info.ssrc = 77;
    ASSERT_TRUE(endpoint->AddPublishedTrack("unsupported", std::make_shared<RtpTrackDescription>(info)));
    ASSERT_TRUE(endpoint->Start());
    const std::vector<uint8_t> packet{0x80, 111, 0, 1, 0, 0, 0, 1,
        0, 0, 0, 77, 0xf8, 0xff, 0xfe};
    WorkJob job;
    job.payload = common::SharedBuffer::TryCopy(packet.data(), packet.size());
    endpoint->OnRtp(job);
    EXPECT_FALSE(endpoint->GetReceiveQualitySnapshot(77));
}

TEST(EndpointComponents, SrBeforeRtpCreatesReceiverAndRegistersQualityBeforeEvaluating)
{
    auto endpoint = std::make_shared<media::SfuEndpoint>(903);
    ::TrackInfo info;
    info.track_index = 0; info.type = TrackAudio; info.codec_id = CodecId::OPUS;
    info.codec_name = "opus"; info.payload_type = 111;
    info.clock_rate = 48000; info.channels = 2; info.ssrc = 77;
    ASSERT_TRUE(endpoint->AddPublishedTrack("audio", std::make_shared<RtpTrackDescription>(info)));
    ASSERT_TRUE(endpoint->Start());
    EXPECT_FALSE(endpoint->GetReceiveQualitySnapshot(77));
    unsigned reports = 0;
    endpoint->SetRtcpSendCallback([&](const uint8_t*, size_t) { ++reports; return true; });
    std::vector<uint8_t> sr(28, 0);
    sr[0] = 0x80; sr[1] = 200; sr[3] = 6; sr[7] = 77;
    sr[8] = 0xe9; sr[12] = 1;
    WorkJob job;
    job.payload = common::SharedBuffer::TryCopy(sr.data(), sr.size());
    endpoint->OnRtcp(job);
    const auto snapshot = endpoint->GetReceiveQualitySnapshot(77);
    ASSERT_TRUE(snapshot);
    EXPECT_GT(snapshot->update_time_ms, 0U);
    EXPECT_TRUE(snapshot->smoothed.loss_rate);
    EXPECT_EQ(reports, 1U);
}
}
