#include <gtest/gtest.h>
#include "DownlinkBandwidthAllocator.h"
#include "VideoResumeBuffer.h"
#include "RtpSenderTrack.h"
#include "SendSideController.h"
#include "RtpTransportCcExtension.h"
#include "Rtp/RtpForwardingState.h"
#include "Rtp/RtpSenderFeedback.h"

namespace
{
using namespace media;

TEST(RtpForwardingState, ResumesAcrossSequenceAndTimestampWrapAndRejectsOldPackets)
{
    rtsp::RtpForwardingState state;
    uint16_t seq; uint32_t ts; int64_t source_ts;
    ASSERT_TRUE(state.Prepare(65535, 0xfffffff0U, seq, ts, source_ts));
    EXPECT_EQ(seq, 65535);
    state.OnMediaSent(seq, ts, source_ts);
    state.BeginDiscontinuity();
    ASSERT_TRUE(state.Prepare(10, 0x20, seq, ts, source_ts));
    EXPECT_EQ(seq, 0);
    EXPECT_EQ(ts, 0x20U);
    state.OnMediaSent(seq, ts, source_ts);
    EXPECT_FALSE(state.Prepare(9, 0x20, seq, ts, source_ts));
    const auto pad = state.NextPaddingSequence();
    EXPECT_EQ(pad, 1);
    state.OnPaddingSent(pad);
    ASSERT_TRUE(state.Prepare(11, 0x30, seq, ts, source_ts));
    EXPECT_EQ(seq, 2);
}

TEST(RtpSenderFeedback, KeyframeRequestsAreThrottledAndRecoverAfterClockRollback)
{
    rtsp::RtpSenderFeedback feedback;
    EXPECT_TRUE(feedback.AllowKeyFrameRequest(100));
    EXPECT_FALSE(feedback.AllowKeyFrameRequest(1099));
    EXPECT_TRUE(feedback.AllowKeyFrameRequest(1100));
    EXPECT_TRUE(feedback.AllowKeyFrameRequest(50));
    EXPECT_FALSE(feedback.AllowKeyFrameRequest(51));
}

DownlinkBandwidthBudget Budget(uint32_t rate, uint64_t at, uint64_t epoch = 1)
{ return {epoch, at, true, true, rate}; }

TEST(DownlinkAllocator, ReservesAudioAndAllocatesPriorityBeforeSmallerVideos)
{
    DownlinkBandwidthAllocator allocator({85, 20, 0, 0, 1500});
    const std::vector<DownlinkDemand> demands{
        {"audio", true, true, 0, 50000}, {"screen", false, true, 200, 600000},
        {"camera", false, true, 128, 400000}, {"thumbnail", false, true, 1, 180000}};
    const auto result = allocator.Allocate(Budget(1000000, 1000), demands, 1000);
    ASSERT_EQ(result.size(), 4U);
    EXPECT_TRUE(result[0].allowed);
    EXPECT_TRUE(result[1].allowed);
    EXPECT_FALSE(result[2].allowed);
    EXPECT_TRUE(result[3].allowed);
}

TEST(DownlinkAllocator, HysteresisRejectsInitialStaleAndOldPathBudgets)
{
    DownlinkBandwidthAllocator allocator;
    std::vector<DownlinkDemand> demand{{"video", false, true, 1, 500000}};
    auto budget = Budget(100000, 1000);
    EXPECT_TRUE(allocator.Allocate(budget, demand, 1000)[0].allowed);
    EXPECT_TRUE(allocator.Allocate(budget, demand, 1299)[0].allowed);
    EXPECT_FALSE(allocator.Allocate(budget, demand, 1300)[0].allowed);
    budget = Budget(1000000, 1400);
    EXPECT_FALSE(allocator.Allocate(budget, demand, 1400)[0].allowed);
    budget.estimate_valid = false;
    EXPECT_FALSE(allocator.Allocate(budget, demand, 2400)[0].allowed);
    budget = Budget(1000000, 2400, 2);
    EXPECT_FALSE(allocator.Allocate(budget, demand, 2400)[0].allowed);
    EXPECT_FALSE(allocator.Allocate(budget, demand, 3399)[0].allowed);
    EXPECT_TRUE(allocator.Allocate(budget, demand, 3400)[0].allowed);
    // A stale zero measurement cannot pause an active stream.
    EXPECT_TRUE(allocator.Allocate(Budget(1, 0, 2), demand, 4000)[0].allowed);
}

TEST(DownlinkAllocator, ResumeRequiresHeadroomAndIneligibleTracksUseNoCapacity)
{
    DownlinkBandwidthAllocator allocator({100, 20, 0, 0, 1500});
    std::vector<DownlinkDemand> demands{{"user-paused", false, false, 255, 900000},
        {"video", false, true, 128, 500000}};
    EXPECT_TRUE(allocator.Allocate(Budget(600000, 0), demands, 0)[1].allowed);
    EXPECT_FALSE(allocator.Allocate(Budget(400000, 1), demands, 1)[1].allowed);
    EXPECT_FALSE(allocator.Allocate(Budget(550000, 2), demands, 2)[1].allowed);
    EXPECT_TRUE(allocator.Allocate(Budget(600000, 3), demands, 3)[1].allowed);
    allocator.Remove("video");
    EXPECT_TRUE(allocator.Allocate(Budget(550000, 4), demands, 4)[1].allowed);
}

TEST(DownlinkAllocator, SourceRateSurvivesSubscriberPausesAndPublisherSilence)
{
    SourceBitrateEstimator estimator;
    estimator.Observe(1000, 0);
    estimator.Observe(1000, 500);
    EXPECT_EQ(estimator.Bitrate(), 32000U);
    estimator.Observe(1000, 10000);
    EXPECT_EQ(estimator.Bitrate(), 32000U);
}

TEST(DownlinkAllocator, PriorityChangeWaitsForStillForwardingTrackToPause)
{
    DownlinkBandwidthAllocator allocator({100, 0, 300, 0, 1500});
    std::vector<DownlinkDemand> demands{{"high", false, true, 1, 500000},
                                        {"low", false, true, 2, 500000}};
    allocator.Allocate(Budget(500000, 0), demands, 0);
    auto result = allocator.Allocate(Budget(500000, 300), demands, 300);
    ASSERT_FALSE(result[0].allowed);
    ASSERT_TRUE(result[1].allowed);
    demands[0].priority = 3;
    result = allocator.Allocate(Budget(500000, 301), demands, 301);
    EXPECT_FALSE(result[0].allowed);
    EXPECT_TRUE(result[1].allowed);
    result = allocator.Allocate(Budget(500000, 601), demands, 601);
    EXPECT_TRUE(result[0].allowed);
    EXPECT_FALSE(result[1].allowed);
}

std::vector<uint8_t> Rtp(uint16_t seq, uint32_t stamp, std::initializer_list<uint8_t> payload, bool marker = true)
{
    std::vector<uint8_t> packet{0x80, uint8_t(96 | (marker ? 0x80 : 0)), uint8_t(seq >> 8), uint8_t(seq),
        uint8_t(stamp >> 24), uint8_t(stamp >> 16), uint8_t(stamp >> 8), uint8_t(stamp), 0, 0, 0, 77};
    packet.insert(packet.end(), payload);
    return packet;
}

TEST(VideoResume, ReordersWholeIdrAndRejectsPartialAndNonFirstSlices)
{
    rtsp::VideoResumeBuffer recovery;
    auto middle = Rtp(0, 90000, {0x7c, 0x05, 0x11}, false);
    auto tail = Rtp(1, 90000, {0x7c, 0x45, 0x22});
    auto head = Rtp(65535, 90000, {0x7c, 0x85, 0x80}, false);
    EXPECT_TRUE(recovery.Push(CodecId::H264, tail.data(), tail.size(), 0).empty());
    EXPECT_TRUE(recovery.Push(CodecId::H264, head.data(), head.size(), 1).empty());
    auto result = recovery.Push(CodecId::H264, middle.data(), middle.size(), 2);
    ASSERT_EQ(result.size(), 3U);
    EXPECT_EQ(result[0], head);
    EXPECT_EQ(result[2], tail);
    auto non_first = Rtp(10, 100000, {0x65, 0x40});
    EXPECT_TRUE(recovery.Push(CodecId::H264, non_first.data(), non_first.size(), 3).empty());
}

TEST(VideoResume, PreservesChangedParametersAndBoundsIncompleteFrames)
{
    rtsp::VideoResumeBuffer recovery;
    auto sps = Rtp(100, 1000, {0x67, 0x42});
    auto pps = Rtp(101, 1000, {0x68, 0xee});
    recovery.Push(CodecId::H264, sps.data(), sps.size(), 0);
    recovery.Push(CodecId::H264, pps.data(), pps.size(), 1);
    recovery.Restart();
    auto idr = Rtp(1000, 90000, {0x65, 0x80});
    auto result = recovery.Push(CodecId::H264, idr.data(), idr.size(), 2);
    ASSERT_EQ(result.size(), 3U);
    EXPECT_EQ(result[0][12], 0x67);
    EXPECT_EQ(result[1][12], 0x68);
    EXPECT_EQ((uint16_t(result[0][2]) << 8) | result[0][3], 998);
    auto first = Rtp(2000, 180000, {0x7c, 0x85, 0x80}, false);
    auto last = Rtp(2001, 180000, {0x7c, 0x45, 0x11});
    EXPECT_TRUE(recovery.Push(CodecId::H264, first.data(), first.size(), 10).empty());
    EXPECT_TRUE(recovery.Push(CodecId::H264, last.data(), last.size(), 210).empty());
    EXPECT_TRUE(recovery.Push(CodecId::H264, first.data(), first.size(), 211).empty());
    // Nested aggregation headers must be rejected without recursive parsing.
    auto malformed = Rtp(2002, 200000, {24, 0, 2, 24, 0});
    EXPECT_TRUE(recovery.Push(CodecId::H264, malformed.data(), malformed.size(), 220).empty());
}

TEST(VideoResume, H265WaitsForAnIdrFirstSliceAndCompleteFragmentRun)
{
    rtsp::VideoResumeBuffer recovery;
    auto cra = Rtp(1, 1000, {uint8_t(21 << 1), 1, 0x80});
    EXPECT_TRUE(recovery.Push(CodecId::H265, cra.data(), cra.size(), 0).empty());
    auto first = Rtp(2, 2000, {uint8_t(49 << 1), 1, uint8_t(0x80 | 19), 0x80}, false);
    auto last = Rtp(3, 2000, {uint8_t(49 << 1), 1, uint8_t(0x40 | 19), 0x11});
    EXPECT_TRUE(recovery.Push(CodecId::H265, first.data(), first.size(), 1).empty());
    EXPECT_EQ(recovery.Push(CodecId::H265, last.data(), last.size(), 2).size(), 2U);
}

TEST(VideoResume, ReplaysNewerIndividualParametersAfterOlderAggregates)
{
    rtsp::VideoResumeBuffer recovery;
    auto aggregate = Rtp(1, 1000, {0x78, 0, 2, 0x67, 0x42, 0, 2, 0x68, 0xee});
    auto new_sps = Rtp(2, 2000, {0x67, 0x64});
    recovery.Push(CodecId::H264, aggregate.data(), aggregate.size(), 0);
    recovery.Push(CodecId::H264, new_sps.data(), new_sps.size(), 1);
    recovery.Restart();
    auto idr = Rtp(3, 3000, {0x65, 0x80});
    const auto result = recovery.Push(CodecId::H264, idr.data(), idr.size(), 2);
    ASSERT_EQ(result.size(), 3U);
    EXPECT_EQ(result[0][12], 0x78);
    EXPECT_EQ(result[1][12], 0x67);
    EXPECT_EQ(result[1][13], 0x64);
}

class SenderTransport : public IMediaTransport
{
public:
    uint64_t Id() const noexcept override { return 1; }
    MediaTransportProtocol Protocol() const noexcept override { return MediaTransportProtocol::Udp; }
    MediaTransportState State() const noexcept override { return MediaTransportState::Connected; }
    SendResult Send(MediaPacketType, const uint8_t* data, size_t size, bool) override
    { packets.emplace_back(data, data + size); return SendResult::Ok; }
    void SetPacketSink(std::weak_ptr<IMediaPacketSink>) override {}
    void Close() override {}
    std::shared_ptr<SendSideController> GetSendSideController() override { return controller; }
    std::shared_ptr<SendSideController> controller = std::make_shared<SendSideController>();
    std::vector<std::vector<uint8_t>> packets;
};
uint16_t Sequence(const std::vector<uint8_t>& p) { return (uint16_t(p[2]) << 8) | p[3]; }

TEST(DownlinkSender, RecoverySkipsIntentionalGapsRetainsNetworkGapsAndClearsOldNacks)
{
    auto transport = std::make_shared<SenderTransport>();
    rtsp::RtpSenderTrackConfig config;
    config.local_ssrc = 99;
    config.transport_cc_extension_id = 3;
    rtsp::RtpSenderTrack sender(config, transport);
    auto packet = Rtp(65535, 1000, {0x65, 0x80});
    ASSERT_TRUE(sender.InputRtpPacket(packet.data(), packet.size()));
    sender.BeginDiscontinuity();
    sender.OnRtcpNack({65535});
    EXPECT_EQ(transport->packets.size(), 1U);
    ASSERT_TRUE(sender.SendPadding(240));
    EXPECT_EQ(Sequence(transport->packets.back()), 0);
    packet = Rtp(1000, 90000, {0x65, 0x80});
    ASSERT_TRUE(sender.InputRtpPacket(packet.data(), packet.size()));
    EXPECT_EQ(Sequence(transport->packets.back()), 1);
    auto late = Rtp(999, 89999, {0x61, 0x80});
    EXPECT_FALSE(sender.InputRtpPacket(late.data(), late.size()));
    packet = Rtp(1002, 93000, {0x61, 0x80});
    ASSERT_TRUE(sender.InputRtpPacket(packet.data(), packet.size()));
    EXPECT_EQ(Sequence(transport->packets.back()), 3); // Real loss remains visible.
    sender.OnRtcpNack({1});
    EXPECT_EQ(Sequence(transport->packets.back()), 1);
    uint16_t first_twcc = 0, retry_twcc = 0;
    ASSERT_TRUE(rtsp::RtpTransportCcExtension::Read(transport->packets[2].data(), transport->packets[2].size(), 3, &first_twcc));
    ASSERT_TRUE(rtsp::RtpTransportCcExtension::Read(transport->packets.back().data(), transport->packets.back().size(), 3, &retry_twcc));
    EXPECT_NE(first_twcc, retry_twcc);
}

TEST(DownlinkSender, ProbesBeforeFirstMediaDoNotCreateASequenceGap)
{
    auto transport = std::make_shared<SenderTransport>();
    rtsp::RtpSenderTrackConfig config;
    config.local_ssrc = 99; config.transport_cc_extension_id = 3;
    rtsp::RtpSenderTrack sender(config, transport);
    EXPECT_FALSE(sender.SendPadding(256));
    ASSERT_TRUE(sender.SendPadding(240));
    sender.BeginDiscontinuity();
    auto packet = Rtp(500, 0xfffff000, {0x65, 0x80});
    ASSERT_TRUE(sender.InputRtpPacket(packet.data(), packet.size()));
    EXPECT_EQ(Sequence(transport->packets.back()), 1);
    ASSERT_TRUE(sender.SendPadding(240));
    packet = Rtp(501, 0x1000, {0x61, 0x80});
    ASSERT_TRUE(sender.InputRtpPacket(packet.data(), packet.size()));
    EXPECT_EQ(Sequence(transport->packets.back()), 3);
}

TEST(DownlinkSender, RecoveryFloorSurvivesLongRunningTimestampWrap)
{
    auto transport = std::make_shared<SenderTransport>();
    rtsp::RtpSenderTrackConfig config;
    config.local_ssrc = 99;
    rtsp::RtpSenderTrack sender(config, transport);
    auto packet = Rtp(1, 0xfffff000, {0x65, 0x80});
    ASSERT_TRUE(sender.InputRtpPacket(packet.data(), packet.size()));
    sender.BeginDiscontinuity();
    uint32_t timestamp = 0x1000;
    for (uint16_t i = 0; i < 20; ++i)
    {
        packet = Rtp(100 + i, timestamp, {0x65, 0x80});
        ASSERT_TRUE(sender.InputRtpPacket(packet.data(), packet.size()));
        EXPECT_EQ(Sequence(transport->packets.back()), uint16_t(2 + i));
        timestamp += 0x10000000;
    }
}

TEST(DownlinkBudget, ObserversCoexistWithApplicationCallbacksAndMayReadOrUnregister)
{
    SendSideController controller;
    int app = 0, observer = 0;
    controller.SetUpdateCallback([&](const auto&) { ++app; });
    SendSideController::BudgetObserverId id = 0;
    id = controller.AddBudgetObserver([&] {
        ++observer;
        EXPECT_FALSE(controller.GetBandwidthBudget().estimate_valid);
        controller.RemoveBudgetObserver(id);
    });
    auto before = controller.GetBandwidthBudget();
    controller.OnReceiverFeedback({}); // RR has no initialized TWCC capacity.
    EXPECT_EQ(app, 1);
    EXPECT_EQ(observer, 1);
    controller.OnNetworkRouteChanged();
    EXPECT_EQ(app, 2);
    EXPECT_EQ(observer, 1);
    EXPECT_GT(controller.GetBandwidthBudget().path_epoch, before.path_epoch);
}
}
