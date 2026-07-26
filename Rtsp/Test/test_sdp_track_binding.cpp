#include <gtest/gtest.h>

#include "RtspMediaSession.h"
#include "Sdp.h"

namespace
{
std::shared_ptr<MediaSession> Apply(const std::string& text)
{
    auto parsed = sdp::Sdp::Parse(text);
    EXPECT_TRUE(parsed.ok) << parsed.message;
    if (!parsed.ok)
    {
        return nullptr;
    }

    auto session = MediaSession::CreateNew("live/test");
    std::string error;
    EXPECT_TRUE(session->ApplySdp(parsed.session, &error)) << error;
    return session;
}

constexpr const char* kSessionHeader =
    "v=0\r\n"
    "o=- 1 1 IN IP4 127.0.0.1\r\n"
    "s=test\r\n"
    "t=0 0\r\n";
}

TEST(SdpTrackBinding, SelectsPayloadInMediaOrderAndMatchingFmtp)
{
    auto session = Apply(std::string(kSessionHeader) +
        "m=audio 0 RTP/AVP 111 0\r\n"
        "a=rtpmap:0 PCMU/8000\r\n"
        "a=rtpmap:111 opus/48000/2\r\n"
        "a=fmtp:0 ignored=1\r\n"
        "a=fmtp:111 minptime=10\r\n"
        "a=control:trackID=audio\r\n");
    ASSERT_NE(session, nullptr);

    TrackInfo info;
    ASSERT_TRUE(session->GetTrackInfo(0, &info));
    EXPECT_EQ(info.type, TrackAudio);
    EXPECT_EQ(info.payload_type, 111);
    EXPECT_EQ(info.codec_id, CodecId::OPUS);
    EXPECT_EQ(info.clock_rate, 48000u);
    EXPECT_EQ(info.channels, 2);
    EXPECT_EQ(info.fmtp, "minptime=10");
}

TEST(SdpTrackBinding, SupportsStaticPcmuWithoutRtpmap)
{
    auto session = Apply(std::string(kSessionHeader) +
        "m=audio 0 RTP/AVP 0\r\n"
        "a=control:trackID=audio\r\n");
    ASSERT_NE(session, nullptr);

    TrackInfo info;
    ASSERT_TRUE(session->GetTrackInfo(0, &info));
    EXPECT_EQ(info.payload_type, 0);
    EXPECT_EQ(info.codec_id, CodecId::PCMU);
    EXPECT_EQ(info.clock_rate, 8000u);
    EXPECT_EQ(info.channels, 1);
}

TEST(SdpTrackBinding, MatchesAbsoluteAndRelativeControl)
{
    auto session = Apply(std::string(kSessionHeader) +
        "m=audio 0 RTP/AVP 8\r\n"
        "a=control:rtsp://camera/live/test/trackID=audio\r\n");
    ASSERT_NE(session, nullptr);

    EXPECT_NE(session->GetRtpTrack("trackID=audio"), nullptr);
    EXPECT_NE(session->GetRtpTrack("rtsp://camera/live/test/trackID=audio"), nullptr);
}

TEST(SdpTrackBinding, PayloadLookupIsScopedToTrack)
{
    auto session = Apply(std::string(kSessionHeader) +
        "m=audio 0 RTP/AVP 96\r\n"
        "a=rtpmap:96 opus/48000/2\r\n"
        "a=control:trackID=audio\r\n"
        "m=video 0 RTP/AVP 96\r\n"
        "a=rtpmap:96 H264/90000\r\n"
        "a=control:trackID=video\r\n");
    ASSERT_NE(session, nullptr);

    PayloadTypeInfo audio;
    PayloadTypeInfo video;
    ASSERT_TRUE(session->FindPayloadType(0, 96, &audio));
    ASSERT_TRUE(session->FindPayloadType(1, 96, &video));
    EXPECT_EQ(audio.track_type, StreamTrackType::Audio);
    EXPECT_EQ(audio.codec_type, CodecType::OPUS);
    EXPECT_EQ(video.track_type, StreamTrackType::Video);
    EXPECT_EQ(video.codec_type, CodecType::H264);
}
