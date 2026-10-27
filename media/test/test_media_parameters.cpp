#include <gtest/gtest.h>

#include "CodecNames.h"
#include "MediaStreamRegistry.h"
#include "MediaTrack.h"
#include "MediaStatsCore.h"
#include "MetricsCalculator.h"
#include "RtpTrackBinding.h"

TEST(MediaParameters, CodecAliasesHaveOneIdentity)
{
    EXPECT_EQ(media::CodecTypeFromName("AaC"), media::CodecType::AAC);
    EXPECT_EQ(media::CodecTypeFromName("mpeg4-generic"), media::CodecType::AAC);
    EXPECT_EQ(media::CodecTypeFromName("HeVc"), media::CodecType::H265);
    EXPECT_EQ(media::CodecTypeFromName("oPuS"), media::CodecType::Opus);
    EXPECT_EQ(media::CodecTypeFromName("unknown"), media::CodecType::Unknown);
}

TEST(MediaParameters, SelectsSupportedCandidateAndProjectsGenericAudio)
{
    media::RtpTrackContext track;
    track.media_index = 7;
    track.media_type = media::MediaType::Audio;
    track.payloads = {{100, media::CodecType::VP8, 90000, 0, "VP8", ""},
                      {111, media::CodecType::Opus, 48000, 2, "opus", "minptime=10"}};
    std::size_t selected = 99;
    ASSERT_TRUE(media::SelectRtpPayload(track, selected));
    EXPECT_EQ(selected, 1U);
    media::MediaStreamTrack description;
    ASSERT_TRUE(media::DescribeMediaTrack(track, selected, description));
    EXPECT_EQ(description.id, 7U);
    EXPECT_EQ(description.type, media::MediaType::Audio);
    EXPECT_EQ(description.codec, media::CodecType::Opus);
    EXPECT_EQ(description.sample_rate, 48000U);
    EXPECT_EQ(description.channels, 2);
    // 拒绝无效选择时保留调用方原有描述。
    EXPECT_FALSE(media::DescribeMediaTrack(track, 99, description));
    EXPECT_EQ(description.id, 7U);
    track.payloads.clear();
    EXPECT_FALSE(media::SelectRtpPayload(track, selected));
    EXPECT_EQ(selected, 1U);
}

TEST(MediaParameters, GenericVideoLifecycleDoesNotRequireRtpClockOrPayloadType)
{
    media::MediaStreamRegistry registry;
    auto stream = std::make_shared<media::MediaStream>("generic/video");
    media::MediaStreamTrack video;
    video.id = 1;
    video.type = media::MediaType::Video;
    video.codec = media::CodecType::H264;
    ASSERT_TRUE(stream->ReplaceTracks({video}));
    ASSERT_TRUE(registry.Register(stream));
    ASSERT_TRUE(stream->BindTrackEndpoint(1, 10));
    ASSERT_TRUE(stream->StartPublishing());
    ASSERT_TRUE(registry.Remove(stream));
    EXPECT_EQ(stream->GetState(), media::MediaStream::State::Closed);
    EXPECT_EQ(registry.Size(), 0U);
    video.codec = media::CodecType::AAC;
    EXPECT_FALSE(video.Valid());
    media::MediaStream invalid("invalid/video");
    EXPECT_FALSE(invalid.ReplaceTracks({video}));
}

TEST(MediaParameters, MetadataSnapshotsOwnTheirValues)
{
    media::TrackMetadata metadata;
    metadata.sid = "camera";
    metadata.type = media::MediaType::Video;
    media::MediaTrack track(metadata);
    const auto id = track.id();
    metadata.sid = "updated";
    track.updateInfo(metadata);
    EXPECT_EQ(id, "camera");
    EXPECT_EQ(track.id(), "updated");
    EXPECT_EQ(track.type(), media::MediaType::Video);
}

TEST(MediaParameters, StatsAndMediaHeadersShareTypesWithoutRedefinition)
{
    media::MediaStatsCore stats;
    media::MediaStatsMeta meta;
    meta.media_type = media::MediaType::Audio;
    const auto first = stats.RegisterTrack(meta);
    const auto second = stats.RegisterTrack(meta);
    EXPECT_NE(first, media::kInvalidMediaStatsHandle);
    EXPECT_NE(first, second);
    stats.UnregisterTrack(first);
    stats.UnregisterTrack(second);
    EXPECT_EQ(media::MetricsCalculator::CalculateBitrateBps(1000, 100), 80000U);
    EXPECT_DOUBLE_EQ(media::MetricsCalculator::JitterToMs(480, 48000), 10.0);
}
