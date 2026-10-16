#include <gtest/gtest.h>

#include "VideoRtpDepacketizerFactory.h"
#include "H264RtpPayloadParser.h"
#include "H26xPacketBuffer.h"

#include <initializer_list>
#include <algorithm>
#include <array>
#include <random>
#include <vector>

namespace
{
using media::CodecType;
using media::EncodedFrame;
using media::IVideoRtpDepacketizer;
using media::VideoRtpDepacketizerFactory;

bool Feed(IVideoRtpDepacketizer& depacketizer, uint16_t seq, uint32_t timestamp,
          bool marker, std::initializer_list<uint8_t> payload,
          int64_t receive_time_ms = 0)
{
    RtpView packet;
    packet.seq = seq;
    packet.ts = timestamp;
    packet.ssrc = 42;
    packet.marker = marker;
    packet.payload = payload.begin();
    packet.payload_len = payload.size();
    packet.receive_time_ms = receive_time_ms;
    return depacketizer.Input(packet);
}
}

TEST(VideoDepacketizerTest, FactoryRejectsUnimplementedOrMismatchedConfigurations)
{
    EXPECT_NE(VideoRtpDepacketizerFactory::Create(CodecType::H264, "h264", 90000), nullptr);
    EXPECT_NE(VideoRtpDepacketizerFactory::Create(CodecType::H264, "", 0), nullptr);
    EXPECT_EQ(VideoRtpDepacketizerFactory::Create(CodecType::H265, "H265", 90000), nullptr);
    EXPECT_EQ(VideoRtpDepacketizerFactory::Create(CodecType::Unknown, "H264", 90000), nullptr);
    EXPECT_EQ(VideoRtpDepacketizerFactory::Create(CodecType::H264, "VP8", 90000), nullptr);
    EXPECT_EQ(VideoRtpDepacketizerFactory::Create(CodecType::H264, "H264", 48000), nullptr);
}

TEST(VideoDepacketizerTest, InterfaceOutputsOwningAnnexBFrameAndMetadata)
{
    auto depacketizer = VideoRtpDepacketizerFactory::Create(CodecType::H264, "H264", 90000);
    ASSERT_NE(depacketizer, nullptr);
    EncodedFrame frame;
    EXPECT_FALSE(depacketizer->PopFrame(frame));
    EXPECT_FALSE(depacketizer->Input(RtpView{}));
    ASSERT_TRUE(Feed(*depacketizer, 10, 90000, true, {0x65, 0xAA}, 100));
    EXPECT_TRUE(depacketizer->HasFrame());
    ASSERT_TRUE(depacketizer->PopFrame(frame));
    EXPECT_FALSE(depacketizer->HasFrame());
    EXPECT_TRUE(frame.Valid());
    EXPECT_TRUE(frame.IsKeyFrame());
    EXPECT_TRUE(frame.IsComplete());
    EXPECT_EQ(frame.info.codec, CodecType::H264);
    EXPECT_EQ(frame.info.media_type, media::MediaType::Video);
    EXPECT_EQ(frame.info.timestamp.time_base_den, 90000);
    EXPECT_EQ(frame.info.timestamp.receive_time_ms, 100);
    EXPECT_EQ(frame.rtp.ssrc, 42U);
    EXPECT_EQ(frame.rtp.packet_count, 1U);
    depacketizer->Reset();
    EXPECT_EQ(frame.buffer.ToVector(), (std::vector<uint8_t>{0, 0, 0, 1, 0x65, 0xAA}));
}

TEST(VideoDepacketizerTest, ReorderedIdrSlicesWaitForGap)
{
    auto depacketizer = VideoRtpDepacketizerFactory::Create(CodecType::H264, "H264", 90000);
    ASSERT_NE(depacketizer, nullptr);
    ASSERT_TRUE(Feed(*depacketizer, 100, 90000, false, {0x65, 0xAA}, 10));
    ASSERT_TRUE(Feed(*depacketizer, 102, 90000, true, {0x65, 0xCC}, 20));
    EXPECT_FALSE(depacketizer->HasFrame());
    ASSERT_TRUE(Feed(*depacketizer, 101, 90000, false, {0x65, 0xBB}, 30));
    EncodedFrame frame;
    ASSERT_TRUE(depacketizer->PopFrame(frame));
    EXPECT_TRUE(frame.IsComplete());
    EXPECT_EQ(frame.rtp.first_sequence, 100U);
    EXPECT_EQ(frame.rtp.last_sequence, 102U);
    EXPECT_EQ(frame.rtp.packet_count, 3U);
    EXPECT_EQ(frame.info.timestamp.receive_time_ms, 30);
    EXPECT_EQ(frame.buffer.ToVector(), (std::vector<uint8_t>{
        0, 0, 0, 1, 0x65, 0xAA, 0, 0, 0, 1, 0x65, 0xBB,
        0, 0, 0, 1, 0x65, 0xCC}));
    EXPECT_FALSE(depacketizer->PopFrame(frame));
}

TEST(VideoDepacketizerTest, GapCompletionReleasesFramesInSequenceWithOwnReceiveTimes)
{
    auto depacketizer = VideoRtpDepacketizerFactory::Create(CodecType::H264, "H264", 90000);
    ASSERT_NE(depacketizer, nullptr);
    ASSERT_TRUE(Feed(*depacketizer, 100, 90000, false, {0x7C, 0x85, 0xAA}, 10));
    ASSERT_TRUE(Feed(*depacketizer, 102, 90000, true, {0x7C, 0x45, 0xCC}, 20));
    ASSERT_TRUE(Feed(*depacketizer, 103, 93000, true, {0x41, 0xDD}, 30));
    EXPECT_FALSE(depacketizer->HasFrame());
    ASSERT_TRUE(Feed(*depacketizer, 101, 90000, false, {0x7C, 0x05, 0xBB}, 40));
    EncodedFrame first, second;
    ASSERT_TRUE(depacketizer->PopFrame(first));
    ASSERT_TRUE(depacketizer->PopFrame(second));
    EXPECT_EQ(first.rtp.rtp_timestamp, 90000U);
    EXPECT_EQ(second.rtp.rtp_timestamp, 93000U);
    EXPECT_EQ(first.info.timestamp.receive_time_ms, 40);
    EXPECT_EQ(second.info.timestamp.receive_time_ms, 30);
    EXPECT_TRUE(second.frame_type == media::EncodedFrameType::Delta);
    EXPECT_FALSE(depacketizer->HasFrame());
}

TEST(VideoDepacketizerTest, NewTimestampKeyframeCanRecoverAfterLoss)
{
    auto depacketizer = VideoRtpDepacketizerFactory::Create(CodecType::H264, "H264", 90000);
    ASSERT_NE(depacketizer, nullptr);
    ASSERT_TRUE(Feed(*depacketizer, 100, 90000, false, {0x7C, 0x85, 0xAA}));
    ASSERT_TRUE(Feed(*depacketizer, 103, 93000, true, {0x65, 0xBB}));
    EncodedFrame frame;
    ASSERT_TRUE(depacketizer->PopFrame(frame));
    EXPECT_EQ(frame.rtp.first_sequence, 103U);
    EXPECT_EQ(frame.rtp.rtp_timestamp, 93000U);
    EXPECT_FALSE(Feed(*depacketizer, 101, 90000, true, {0x7C, 0x45, 0xCC}));
    EXPECT_FALSE(depacketizer->HasFrame());
}

TEST(VideoDepacketizerTest, ResetDropsPendingFramesAndRestoresNegotiatedParameterSets)
{
    auto depacketizer = VideoRtpDepacketizerFactory::Create(CodecType::H264, "H264", 90000,
        "sprop-parameter-sets=Z0IAH5WoFAFuQA==,aM4G4g==");
    ASSERT_NE(depacketizer, nullptr);
    ASSERT_TRUE(Feed(*depacketizer, 100, 90000, true, {0x65, 0xAA}));
    EXPECT_TRUE(depacketizer->HasFrame());
    depacketizer->Reset();
    EXPECT_FALSE(depacketizer->HasFrame());
    ASSERT_TRUE(Feed(*depacketizer, 1, 3000, true, {0x65, 0xBB}));
    EncodedFrame frame;
    ASSERT_TRUE(depacketizer->PopFrame(frame));
    EXPECT_EQ(frame.rtp.first_sequence, 1U);
    EXPECT_TRUE(frame.video.has_sps);
    EXPECT_TRUE(frame.video.has_pps);
    EXPECT_TRUE(frame.video.parameter_sets_injected);
    EXPECT_GT(frame.video.width, 0);
    EXPECT_GT(frame.video.height, 0);
    EXPECT_GT(frame.Size(), 6U);
}

TEST(VideoDepacketizerTest, LaterParameterSetDoesNotAffectEarlierBufferedFrame)
{
    auto depacketizer = VideoRtpDepacketizerFactory::Create(CodecType::H264, "H264", 90000);
    ASSERT_NE(depacketizer, nullptr);
    ASSERT_TRUE(Feed(*depacketizer, 100, 90000, false, {0x7C, 0x85, 0xAA}));
    ASSERT_TRUE(Feed(*depacketizer, 102, 90000, true, {0x7C, 0x45, 0xCC}));
    // PPS cannot independently resynchronize, so it waits behind the gap.
    ASSERT_TRUE(Feed(*depacketizer, 103, 93000, true, {0x68, 0xC0}));
    ASSERT_TRUE(Feed(*depacketizer, 101, 90000, false, {0x7C, 0x05, 0xBB}));
    EncodedFrame first, config;
    ASSERT_TRUE(depacketizer->PopFrame(first));
    EXPECT_FALSE(first.video.has_pps);
    EXPECT_FALSE(first.video.parameter_sets_injected);
    ASSERT_TRUE(depacketizer->PopFrame(config));
    EXPECT_TRUE(config.IsConfigFrame());
    EXPECT_TRUE(config.video.has_pps);
    ASSERT_TRUE(Feed(*depacketizer, 104, 96000, true, {0x65, 0xDD}));
    EncodedFrame next;
    ASSERT_TRUE(depacketizer->PopFrame(next));
    EXPECT_TRUE(next.video.has_pps);
    EXPECT_TRUE(next.video.parameter_sets_injected);
}

TEST(VideoDepacketizerTest, StartupLastSliceFirstWaitsForFirstSlice)
{
    auto depacketizer = VideoRtpDepacketizerFactory::Create(CodecType::H264, "H264", 90000);
    ASSERT_NE(depacketizer, nullptr);
    // first_mb_in_slice: 2 (011...), 1 (010...), then 0 (1...).
    ASSERT_TRUE(Feed(*depacketizer, 102, 90000, true, {0x65, 0x60}));
    EXPECT_FALSE(depacketizer->HasFrame());
    ASSERT_TRUE(Feed(*depacketizer, 101, 90000, false, {0x65, 0x40}));
    EXPECT_FALSE(depacketizer->HasFrame());
    ASSERT_TRUE(Feed(*depacketizer, 100, 90000, false, {0x65, 0x80}));
    EncodedFrame frame;
    ASSERT_TRUE(depacketizer->PopFrame(frame));
    EXPECT_EQ(frame.rtp.first_sequence, 100U);
    EXPECT_EQ(frame.rtp.packet_count, 3U);
    EXPECT_EQ(frame.buffer.ToVector(), (std::vector<uint8_t>{
        0, 0, 0, 1, 0x65, 0x80, 0, 0, 0, 1, 0x65, 0x40,
        0, 0, 0, 1, 0x65, 0x60}));
    EXPECT_FALSE(depacketizer->HasFrame());
}

TEST(VideoDepacketizerTest, RecoveryCannotStartAtLaterSliceOfNewTimestamp)
{
    auto depacketizer = VideoRtpDepacketizerFactory::Create(CodecType::H264, "H264", 90000);
    ASSERT_NE(depacketizer, nullptr);
    ASSERT_TRUE(Feed(*depacketizer, 90, 87000, true, {0x65, 0x80}));
    EncodedFrame frame;
    ASSERT_TRUE(depacketizer->PopFrame(frame));
    ASSERT_TRUE(Feed(*depacketizer, 103, 90000, true, {0x65, 0x40}));
    EXPECT_FALSE(depacketizer->HasFrame());
    ASSERT_TRUE(Feed(*depacketizer, 102, 90000, false, {0x65, 0x80}));
    ASSERT_TRUE(depacketizer->PopFrame(frame));
    EXPECT_EQ(frame.rtp.first_sequence, 102U);
    EXPECT_EQ(frame.rtp.last_sequence, 103U);
}

TEST(VideoDepacketizerTest, StartupAtLaterFragmentedSliceDoesNotProducePartialAu)
{
    auto depacketizer = VideoRtpDepacketizerFactory::Create(CodecType::H264, "H264", 90000);
    ASSERT_NE(depacketizer, nullptr);
    ASSERT_TRUE(Feed(*depacketizer, 102, 90000, false, {0x7C, 0x85, 0x40}));
    ASSERT_TRUE(Feed(*depacketizer, 103, 90000, true, {0x7C, 0x45, 0xBB}));
    EXPECT_FALSE(depacketizer->HasFrame());
    ASSERT_TRUE(Feed(*depacketizer, 100, 90000, false, {0x7C, 0x85, 0x80}));
    ASSERT_TRUE(Feed(*depacketizer, 101, 90000, false, {0x7C, 0x45, 0xAA}));
    EncodedFrame frame;
    ASSERT_TRUE(depacketizer->PopFrame(frame));
    EXPECT_EQ(frame.rtp.packet_count, 4U);
    EXPECT_EQ(frame.buffer.ToVector(), (std::vector<uint8_t>{
        0, 0, 0, 1, 0x65, 0x80, 0xAA, 0, 0, 0, 1, 0x65, 0x40, 0xBB}));
}

TEST(VideoDepacketizerTest, FramePacketLimitRejectsWholeAuAndAllowsNextKeyframe)
{
    for (const int count : {512, 513, 600})
    {
        SCOPED_TRACE(count);
        auto depacketizer = VideoRtpDepacketizerFactory::Create(CodecType::H264, "H264", 90000);
        ASSERT_NE(depacketizer, nullptr);
        for (int i = 0; i < count; ++i)
        {
            ASSERT_TRUE(Feed(*depacketizer, 100 + i, 90000, i + 1 == count,
                             {0x65, static_cast<uint8_t>(i == 0 ? 0x80 : 0x40)}));
        }
        EncodedFrame frame;
        if (count == 512)
        {
            ASSERT_TRUE(depacketizer->PopFrame(frame));
            EXPECT_EQ(frame.rtp.first_sequence, 100U);
            EXPECT_EQ(frame.rtp.packet_count, 512U);
        }
        EXPECT_FALSE(depacketizer->HasFrame());
        ASSERT_TRUE(Feed(*depacketizer, 100 + count, 93000, true, {0x65, 0x80}));
        ASSERT_TRUE(depacketizer->PopFrame(frame));
        EXPECT_EQ(frame.rtp.rtp_timestamp, 93000U);
        EXPECT_EQ(frame.rtp.packet_count, 1U);
    }
}

TEST(VideoDepacketizerTest, FuFragmentsMustKeepNalTypeAndNri)
{
    for (const uint8_t end_header : {0x41, 0x45})
    {
        SCOPED_TRACE(end_header);
        auto depacketizer = VideoRtpDepacketizerFactory::Create(CodecType::H264, "H264", 90000);
        ASSERT_NE(depacketizer, nullptr);
        ASSERT_TRUE(Feed(*depacketizer, 100, 90000, false, {0x7C, 0x85, 0x80}));
        // Either change type 5 -> 1 or NRI 3 -> 2 within the NAL.
        const uint8_t indicator = end_header == 0x41 ? 0x7C : 0x5C;
        ASSERT_TRUE(Feed(*depacketizer, 101, 90000, true, {indicator, end_header, 0xBB}));
        EXPECT_FALSE(depacketizer->HasFrame());
        ASSERT_TRUE(Feed(*depacketizer, 102, 93000, true, {0x65, 0x80}));
        EncodedFrame frame;
        ASSERT_TRUE(depacketizer->PopFrame(frame));
        EXPECT_EQ(frame.rtp.rtp_timestamp, 93000U);
    }
}

TEST(VideoDepacketizerTest, ForbiddenBitAndTruncatedPayloadsAreRejected)
{
    auto depacketizer = VideoRtpDepacketizerFactory::Create(CodecType::H264, "H264", 90000);
    ASSERT_NE(depacketizer, nullptr);
    EXPECT_FALSE(Feed(*depacketizer, 100, 90000, true, {0xE5, 0x80}));
    EXPECT_FALSE(Feed(*depacketizer, 101, 90000, true, {0x65}));
    EXPECT_FALSE(Feed(*depacketizer, 102, 90000, true, {0x78, 0, 3, 0x65, 0x80}));
    EXPECT_FALSE(Feed(*depacketizer, 103, 90000, true, {0x7C, 0xC5, 0x80}));
    EXPECT_FALSE(depacketizer->HasFrame());
}

TEST(VideoDepacketizerTest, MissingFuStartOrEndCannotYieldCompleteFrame)
{
    auto depacketizer = VideoRtpDepacketizerFactory::Create(CodecType::H264, "H264", 90000);
    ASSERT_NE(depacketizer, nullptr);
    ASSERT_TRUE(Feed(*depacketizer, 100, 90000, true, {0x7C, 0x45, 0xBB}));
    EXPECT_FALSE(depacketizer->HasFrame());
    ASSERT_TRUE(Feed(*depacketizer, 101, 93000, false, {0x7C, 0x85, 0x80}));
    ASSERT_TRUE(Feed(*depacketizer, 102, 93000, true, {0x7C, 0x05, 0xBB}));
    EXPECT_FALSE(depacketizer->HasFrame());
    ASSERT_TRUE(Feed(*depacketizer, 103, 96000, true, {0x65, 0x80}));
    EncodedFrame frame;
    ASSERT_TRUE(depacketizer->PopFrame(frame));
    EXPECT_EQ(frame.rtp.rtp_timestamp, 96000U);
}

TEST(VideoDepacketizerTest, TimestampChangeDropsAuWithoutMarker)
{
    auto depacketizer = VideoRtpDepacketizerFactory::Create(CodecType::H264, "H264", 90000);
    ASSERT_NE(depacketizer, nullptr);
    ASSERT_TRUE(Feed(*depacketizer, 100, 90000, false, {0x65, 0x80}));
    ASSERT_TRUE(Feed(*depacketizer, 101, 93000, true, {0x65, 0x80}));
    EncodedFrame frame;
    ASSERT_TRUE(depacketizer->PopFrame(frame));
    EXPECT_EQ(frame.rtp.first_sequence, 101U);
    EXPECT_EQ(frame.rtp.rtp_timestamp, 93000U);
    EXPECT_FALSE(depacketizer->HasFrame());
}

TEST(VideoDepacketizerTest, EveryArrivalPermutationPreservesMultisliceBytesAcrossSequenceWrap)
{
    std::array<int, 4> order{0, 1, 2, 3};
    do
    {
        auto depacketizer = VideoRtpDepacketizerFactory::Create(CodecType::H264, "H264", 90000);
        ASSERT_NE(depacketizer, nullptr);
        int received = 0;
        for (const int i : order)
        {
            ASSERT_TRUE(Feed(*depacketizer, static_cast<uint16_t>(65534 + i),
                90000, i == 3, {0x65, static_cast<uint8_t>(i == 0 ? 0x80 : 0x40 + i)}));
            if (++received < 4) EXPECT_FALSE(depacketizer->HasFrame());
        }
        EncodedFrame frame;
        ASSERT_TRUE(depacketizer->PopFrame(frame));
        EXPECT_EQ(frame.rtp.first_sequence, 65534U);
        EXPECT_EQ(frame.rtp.last_sequence, 1U);
        EXPECT_EQ(frame.rtp.packet_count, 4U);
        EXPECT_EQ(frame.buffer.ToVector(), (std::vector<uint8_t>{
            0, 0, 0, 1, 0x65, 0x80, 0, 0, 0, 1, 0x65, 0x41,
            0, 0, 0, 1, 0x65, 0x42, 0, 0, 0, 1, 0x65, 0x43}));
        EXPECT_FALSE(depacketizer->HasFrame());
    } while (std::next_permutation(order.begin(), order.end()));
}

TEST(VideoDepacketizerTest, RandomReorderingAndDuplicatesPreserveExactlyOneAu)
{
    std::mt19937 random(0x264);
    for (int trial = 0; trial < 64; ++trial)
    {
        SCOPED_TRACE(trial);
        const int count = 2 + random() % 15;
        std::vector<int> order;
        std::vector<uint8_t> expected;
        for (int i = 0; i < count; ++i)
        {
            order.push_back(i);
            expected.insert(expected.end(), {0, 0, 0, 1, 0x65,
                static_cast<uint8_t>(i == 0 ? 0x80 : 0x40 + i)});
        }
        std::shuffle(order.begin(), order.end(), random);
        auto depacketizer = VideoRtpDepacketizerFactory::Create(CodecType::H264, "H264", 90000);
        ASSERT_NE(depacketizer, nullptr);
        const uint16_t first_seq = trial & 1 ? 65530 : 100;
        int received = 0;
        for (const int i : order)
        {
            const auto seq = static_cast<uint16_t>(first_seq + i);
            const auto slice = static_cast<uint8_t>(i == 0 ? 0x80 : 0x40 + i);
            ASSERT_TRUE(Feed(*depacketizer, seq, 90000, i + 1 == count, {0x65, slice}));
            // Duplicate packets may be reported as duplicates or late, but
            // neither response may produce an extra AU or corrupt its bytes.
            Feed(*depacketizer, seq, 90000, i + 1 == count, {0x65, slice});
            if (++received < count) EXPECT_FALSE(depacketizer->HasFrame());
        }
        EncodedFrame frame;
        ASSERT_TRUE(depacketizer->PopFrame(frame));
        EXPECT_EQ(frame.buffer.ToVector(), expected);
        EXPECT_EQ(frame.rtp.packet_count, static_cast<uint32_t>(count));
        EXPECT_FALSE(depacketizer->HasFrame());
        EXPECT_FALSE(Feed(*depacketizer, first_seq, 90000, false, {0x65, 0x80}));
    }
}

TEST(VideoDepacketizerTest, DroppingAnyFuFragmentPreventsOutputUntilRecovery)
{
    for (int missing = 0; missing < 4; ++missing)
    {
        SCOPED_TRACE(missing);
        auto depacketizer = VideoRtpDepacketizerFactory::Create(CodecType::H264, "H264", 90000);
        ASSERT_NE(depacketizer, nullptr);
        // Tail first also exercises startup buffering while the start is absent.
        for (const int i : {3, 2, 1, 0})
        {
            if (i == missing) continue;
            const uint8_t header = i == 0 ? 0x85 : (i == 3 ? 0x45 : 0x05);
            ASSERT_TRUE(Feed(*depacketizer, 100 + i, 90000, i == 3, {0x7C, header, 0x80}));
        }
        EXPECT_FALSE(depacketizer->HasFrame());
        ASSERT_TRUE(Feed(*depacketizer, 110, 93000, true, {0x65, 0x80}));
        EncodedFrame frame;
        ASSERT_TRUE(depacketizer->PopFrame(frame));
        EXPECT_EQ(frame.rtp.first_sequence, 110U);
        EXPECT_FALSE(depacketizer->HasFrame());
    }
}

TEST(VideoDepacketizerTest, OversizedAuWithoutMarkerReleasesStorageAndRecovers)
{
    media::H264PacketBuffer buffer;
    media::H264RtpPayloadParser parser;
    for (int i = 0; i <= 2500; ++i)
    {
        const uint8_t payload[] = {0x65, static_cast<uint8_t>(i == 0 || i == 2500 ? 0x80 : 0x40)};
        RtpView view;
        view.seq = 100 + i;
        view.ts = i == 2500 ? 93000 : 90000;
        view.ssrc = 42;
        view.marker = i == 2500;
        view.payload = payload;
        view.payload_len = sizeof(payload);
        media::H264ParsedPacket parsed;
        ASSERT_TRUE(parser.Parse(view, parsed));
        const auto result = buffer.InsertPacket(std::move(parsed));
        EXPECT_LE(buffer.BufferedPacketCount(), 512U);
        if (i == 2500)
        {
            ASSERT_EQ(result.frames.size(), 1U);
            EXPECT_EQ(result.frames[0].timestamp, 93000U);
            EXPECT_EQ(buffer.BufferedPacketCount(), 0U);
        }
        else
        {
            EXPECT_TRUE(result.frames.empty());
            if (i >= 512) EXPECT_EQ(buffer.BufferedPacketCount(), 0U);
        }
    }
}
