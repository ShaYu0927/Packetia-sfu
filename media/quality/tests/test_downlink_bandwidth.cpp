#include <gtest/gtest.h>
#include "DownlinkBandwidthAllocator.h"
#include "VideoResumeBuffer.h"
#include "RtpSenderTrack.h"
#include "SendSideController.h"
#include "PacedSendQueue.h"
#include "RtpTransportCcExtension.h"
#include "Rtp/RtpForwardingState.h"
#include "Rtp/RtpSenderFeedback.h"
#include <algorithm>
#include <limits>
#include <map>
#include <tuple>

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

TEST(DownlinkAllocator, RecoveryBudgetIsTheAllocationBoundaryForOnePausedVideo)
{
    struct Case { uint32_t usable, margin, active, candidate; uint64_t expected; };
    for (const auto& item : {Case{85, 20, 1000000, 300000, 1600000},
                             Case{100, 0, 50000, 300000, 350000},
                             Case{80, 50, 100000, 200000, 500000},
                             Case{85, 20, 1, 1, 3}})
    {
        DownlinkBandwidthAllocator allocator({item.usable, item.margin, 0, 0, 1500});
        const std::vector<DownlinkDemand> demands{
            {"audio", true, true, 0, item.active}, {"video", false, true, 128, item.candidate}};
        ASSERT_FALSE(allocator.Allocate(Budget(1, 1000), demands, 1000)[1].allowed);
        const auto required = allocator.RequiredBudgetToResume(item.active, item.candidate);
        ASSERT_TRUE(required);
        EXPECT_EQ(*required, item.expected);
        ASSERT_FALSE(allocator.Allocate(Budget(static_cast<uint32_t>(*required - 1), 1001), demands, 1001)[1].allowed);
        ASSERT_TRUE(allocator.Allocate(Budget(static_cast<uint32_t>(*required), 1002), demands, 1002)[1].allowed);
    }
}

TEST(DownlinkAllocator, RecoveryBudgetQueryPreservesResumeWaitAndHandlesUnknownAndOverflow)
{
    DownlinkBandwidthAllocator allocator({85, 20, 0, 1000, 1500});
    const std::vector<DownlinkDemand> demands{{"video", false, true, 128, 300000}};
    ASSERT_FALSE(allocator.Allocate(Budget(100000, 1000), demands, 1000)[0].allowed);
    const auto required = allocator.RequiredBudgetToResume(0, 300000);
    ASSERT_TRUE(required);
    const auto rate = static_cast<uint32_t>(*required);
    EXPECT_FALSE(allocator.Allocate(Budget(rate, 1100), demands, 1100)[0].allowed);
    for (int i = 0; i < 10; ++i) EXPECT_EQ(allocator.RequiredBudgetToResume(0, 300000), required);
    EXPECT_FALSE(allocator.Allocate(Budget(rate, 2099), demands, 2099)[0].allowed);
    EXPECT_TRUE(allocator.Allocate(Budget(rate, 2100), demands, 2100)[0].allowed);
    EXPECT_FALSE(allocator.RequiredBudgetToResume(0, 0));
    EXPECT_FALSE(allocator.RequiredBudgetToResume(std::numeric_limits<uint64_t>::max(), 1));
    EXPECT_FALSE(allocator.RequiredBudgetToResume(std::numeric_limits<uint64_t>::max() - 10, 1));
    DownlinkBandwidthAllocator full_budget({100, 0, 0, 0, 1500});
    EXPECT_EQ(full_budget.RequiredBudgetToResume(std::numeric_limits<uint64_t>::max() - 1, 1),
        std::numeric_limits<uint64_t>::max());
}

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
    EXPECT_EQ(result[0].bandwidth_requested_bps, 0U);
    EXPECT_EQ(result[0].bandwidth_delta_bps, 0);
    EXPECT_EQ(result[1].bandwidth_requested_bps, 500000U);
    EXPECT_EQ(result[1].bandwidth_delta_bps, 0);
    result = allocator.Allocate(Budget(500000, 601), demands, 601);
    EXPECT_TRUE(result[0].allowed);
    EXPECT_FALSE(result[1].allowed);
    EXPECT_EQ(result[0].bandwidth_delta_bps, 500000);
    EXPECT_EQ(result[1].bandwidth_delta_bps, -500000);
}

TEST(DownlinkAllocator, CommittedBandwidthTracksHysteresisAndExcludesResumeHeadroom)
{
    DownlinkBandwidthAllocator allocator({100, 20, 300, 1000, 1500});
    const std::vector<DownlinkDemand> demands{
        {"audio", true, true, 0, 50000}, {"video", false, true, 128, 500000}};
    auto result = allocator.Allocate(Budget(600000, 1000), demands, 1000);
    ASSERT_EQ(result.size(), 2U);
    EXPECT_EQ(result[0].bandwidth_requested_bps, 50000U);
    EXPECT_EQ(result[0].bandwidth_delta_bps, 50000);
    EXPECT_EQ(result[1].bandwidth_requested_bps, 500000U);
    EXPECT_EQ(result[1].bandwidth_delta_bps, 500000);

    result = allocator.Allocate(Budget(300000, 1100), demands, 1100);
    EXPECT_TRUE(result[1].allowed); // A provisional pause still consumes bandwidth.
    EXPECT_EQ(result[1].bandwidth_requested_bps, 500000U);
    EXPECT_EQ(result[1].bandwidth_delta_bps, 0);
    result = allocator.Allocate(Budget(300000, 1400), demands, 1400);
    EXPECT_FALSE(result[1].allowed);
    EXPECT_EQ(result[1].bandwidth_requested_bps, 0U);
    EXPECT_EQ(result[1].bandwidth_delta_bps, -500000);

    result = allocator.Allocate(Budget(650000, 1500), demands, 1500);
    EXPECT_FALSE(result[1].allowed);
    EXPECT_EQ(result[1].bandwidth_requested_bps, 0U);
    result = allocator.Allocate(Budget(650000, 2499), demands, 2499);
    EXPECT_FALSE(result[1].allowed);
    result = allocator.Allocate(Budget(650000, 2500), demands, 2500);
    EXPECT_TRUE(result[1].allowed);
    EXPECT_EQ(result[1].bandwidth_requested_bps, 500000U); // Admission reserved 600000.
    EXPECT_EQ(result[1].bandwidth_delta_bps, 500000);
    result = allocator.Allocate(Budget(650000, 2501), demands, 2501);
    EXPECT_EQ(result[0].bandwidth_delta_bps, 0);
    EXPECT_EQ(result[1].bandwidth_delta_bps, 0);
}

TEST(DownlinkAllocator, SingleLayerPlanningIsIndependentOfDemandOrder)
{
    std::vector<DownlinkDemand> demands{
        {"b", false, true, 128, 500000}, {"audio", true, true, 0, 50000},
        {"a", false, true, 128, 500000}};
    using State = std::tuple<bool, uint32_t, int64_t>;
    std::map<std::string, State> expected;
    for (int pass = 0; pass < 2; ++pass)
    {
        DownlinkBandwidthAllocator allocator({100, 20, 0, 0, 1500});
        const auto result = allocator.Allocate(Budget(550000, 0), demands, 0);
        std::map<std::string, State> actual;
        for (const auto& allocation : result)
            actual.emplace(allocation.id, State{allocation.allowed, allocation.bandwidth_requested_bps,
                                                allocation.bandwidth_delta_bps});
        EXPECT_TRUE(std::get<0>(actual.at("a")));
        EXPECT_FALSE(std::get<0>(actual.at("b")));
        if (pass == 0) expected = actual;
        else EXPECT_EQ(actual, expected);
        std::reverse(demands.begin(), demands.end());
    }
}

TEST(DownlinkAllocator, CommittedDemandChangesWithMeasurementsEligibilityAndTrackLifetime)
{
    DownlinkBandwidthAllocator allocator({100, 20, 0, 0, 1500});
    std::vector<DownlinkDemand> demands{{"video", false, true, 128, 500000}};
    EXPECT_EQ(allocator.Allocate(Budget(1000000, 0), demands, 0)[0].bandwidth_delta_bps, 500000);
    demands[0].required_bps = 400000;
    auto result = allocator.Allocate(Budget(1000000, 1), demands, 1);
    EXPECT_TRUE(result[0].allowed);
    EXPECT_EQ(result[0].bandwidth_delta_bps, -100000);
    demands[0].eligible = false;
    result = allocator.Allocate(Budget(1000000, 2), demands, 2);
    EXPECT_EQ(result[0].bandwidth_requested_bps, 0U);
    EXPECT_EQ(result[0].bandwidth_delta_bps, -400000);
    demands[0].eligible = true;
    EXPECT_EQ(allocator.Allocate(Budget(1000000, 3), demands, 3)[0].bandwidth_delta_bps, 400000);
    EXPECT_TRUE(allocator.Allocate(Budget(1000000, 4), {}, 4).empty());
    EXPECT_EQ(allocator.Allocate(Budget(1000000, 5), demands, 5)[0].bandwidth_delta_bps, 400000);
    allocator.Clear();
    EXPECT_EQ(allocator.Allocate(Budget(1000000, 6), demands, 6)[0].bandwidth_delta_bps, 400000);
}

TEST(DownlinkAllocator, UnknownVideoDemandCannotConsumeAFreeCandidate)
{
    DownlinkBandwidthAllocator allocator({100, 20, 0, 0, 1500});
    const std::vector<DownlinkDemand> demands{
        {"unknown", false, true, 255, 0}, {"known", false, true, 128, 500000}};
    const auto result = allocator.Allocate(Budget(500000, 0), demands, 0);
    EXPECT_FALSE(result[0].allowed);
    EXPECT_EQ(result[0].bandwidth_requested_bps, 0U);
    EXPECT_TRUE(result[1].allowed);
    EXPECT_EQ(result[1].bandwidth_requested_bps, 500000U);
}

TEST(DownlinkAllocator, ResumeReservationAndSignedDeltasHandleLargeDemands)
{
    constexpr auto maximum = std::numeric_limits<uint32_t>::max();
    DownlinkBandwidthAllocator allocator({100, 100, 0, 0, 1500});
    std::vector<DownlinkDemand> demands{{"video", false, true, 128, maximum}};
    auto result = allocator.Allocate(Budget(maximum, 0), demands, 0);
    EXPECT_EQ(result[0].bandwidth_delta_bps, static_cast<int64_t>(maximum));
    result = allocator.Allocate(Budget(1, 1), demands, 1);
    EXPECT_FALSE(result[0].allowed);
    EXPECT_EQ(result[0].bandwidth_delta_bps, -static_cast<int64_t>(maximum));
    result = allocator.Allocate(Budget(maximum, 2), demands, 2);
    EXPECT_FALSE(result[0].allowed); // Twice the demand exceeds a uint32_t budget.
    demands[0].required_bps = maximum / 2;
    result = allocator.Allocate(Budget(maximum, 3), demands, 3);
    EXPECT_TRUE(result[0].allowed);
    EXPECT_EQ(result[0].bandwidth_requested_bps, maximum / 2);
    EXPECT_EQ(result[0].bandwidth_delta_bps, static_cast<int64_t>(maximum / 2));
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
// Deterministic socket boundary: accepted packets are paced first, then complete.
class PacedProbeTransport final : public SenderTransport
{
public:
    PacedProbeTransport()
    {
        NetworkControllerConfig config;
        config.bitrate = {100000, 200000, 2000000};
        controller = std::make_shared<SendSideController>(config);
    }
    uint64_t now = 1000, next_task = 0;
    std::map<std::pair<uint64_t, uint64_t>, std::function<void()>> tasks;
    std::vector<uint64_t> send_times;
    media::transport::PacedSendQueue queue{
        [this](uint32_t delay, auto task) {
            tasks.emplace(std::make_pair(now + delay, ++next_task), std::move(task)); return true;
        },
        [this](auto packet) {
            packets.push_back(std::move(packet.bytes));
            send_times.push_back(now);
            if (packet.on_sent) packet.on_sent();
        }, [this] { return controller->GetPacingRate(now); }, [this] { return now; }};
    SendResult SendRtpWithCompletion(const uint8_t* data, size_t size, bool,
                                    std::function<void()> complete) override
    {
        return queue.Send(media::transport::PacedSendQueue::Priority::Media, data, size, {},
            std::move(complete)) == media::transport::PacedSendQueue::Result::Accepted ?
            SendResult::Ok : SendResult::NotWritable;
    }
    void Advance(uint64_t end)
    {
        while (!tasks.empty() && tasks.begin()->first.first <= end)
        {
            auto task = tasks.extract(tasks.begin());
            now = task.key().first;
            task.mapped()();
        }
        now = end;
    }
};

TEST(DownlinkSender, PaddingSocketCompletionsAndEncodedTwccRaiseSharedBudget)
{
    auto transport = std::make_shared<PacedProbeTransport>();
    rtsp::RtpSenderTrackConfig config;
    config.local_ssrc = 99;
    config.transport_cc_extension_id = 3;
    rtsp::RtpSenderTrack sender(config, transport, [&] { return transport->now; });
    auto controller = transport->controller;
    auto decision = controller->GetProbeDecision({1000, controller->PathEpoch(), 0, 400000});
    ASSERT_TRUE(decision.plan);
    const auto plan = controller->StartProbe(*decision.plan, 1000, controller->PathEpoch());
    ASSERT_TRUE(plan);
    uint64_t accounted = 0;
    for (uint64_t tick = 1000; tick < 1500; tick += 20)
    {
        transport->Advance(tick);
        const auto allowance = std::min(plan->max_bytes, uint64_t(plan->bitrate_bps) * (tick - 1000 + 20) / 8000);
        while (accounted + 300 <= allowance)
        {
            ASSERT_TRUE(sender.SendPadding(240, plan->cluster_id));
            accounted += 300;
        }
    }
    transport->Advance(1500);
    controller->OnProbeEvent({ProbeEventType::Finished, *plan, controller->PathEpoch(), 1500, accounted});
    ASSERT_GT(transport->packets.size(), 50U);
    EXPECT_FALSE(controller->GetBandwidthBudget().estimate_valid);

    // One TWCC run-length chunk: all packets received with small deltas.
    auto put16 = [](uint8_t* p, uint16_t v) { p[0] = uint8_t(v >> 8); p[1] = uint8_t(v); };
    std::vector<uint8_t> feedback(22, 0);
    feedback[0] = 0x8f; feedback[1] = 205;
    uint16_t sequence;
    ASSERT_TRUE(rtsp::RtpTransportCcExtension::Read(transport->packets.front().data(),
        transport->packets.front().size(), 3, &sequence));
    put16(feedback.data() + 12, sequence);
    put16(feedback.data() + 14, static_cast<uint16_t>(transport->packets.size()));
    const uint64_t first_receive_ms = 50000 + transport->send_times.front() - 1000;
    const uint32_t reference = static_cast<uint32_t>(first_receive_ms / 64);
    feedback[16] = uint8_t(reference >> 16); feedback[17] = uint8_t(reference >> 8); feedback[18] = uint8_t(reference);
    put16(feedback.data() + 20, 0x2000 | static_cast<uint16_t>(transport->packets.size()));
    uint64_t previous_receive_ms = uint64_t(reference) * 64;
    for (const auto at : transport->send_times)
    {
        const uint64_t receive_ms = 50000 + at - 1000;
        ASSERT_LE((receive_ms - previous_receive_ms) * 4, 255U);
        feedback.push_back(static_cast<uint8_t>((receive_ms - previous_receive_ms) * 4));
        previous_receive_ms = receive_ms;
    }
    while (feedback.size() % 4) feedback.push_back(0);
    put16(feedback.data() + 2, static_cast<uint16_t>(feedback.size() / 4 - 1));
    unsigned updates = 0;
    controller->AddBudgetObserver([&] { ++updates; });
    ASSERT_TRUE(controller->OnRtcpPacket(feedback.data(), feedback.size(), 1550));
    ASSERT_TRUE(controller->LastProbeResult());
    EXPECT_EQ(controller->LastProbeResult()->outcome, ProbeOutcome::Succeeded);
    const auto budget = controller->GetBandwidthBudget();
    EXPECT_TRUE(budget.estimate_valid);
    EXPECT_GT(budget.target_bps, 200000U);
    EXPECT_LT(budget.target_bps, plan->pacing_bitrate_bps);
    EXPECT_EQ(updates, 1U);
    ASSERT_TRUE(controller->OnRtcpPacket(feedback.data(), feedback.size(), 1560));
    EXPECT_EQ(updates, 1U); // Matched history was consumed, so replay cannot grow again.
}

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
