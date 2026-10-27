#include <gtest/gtest.h>

#include "RtspMediaSession.h"
#include "RtpStreamContext.h"
#include "SdpTrackBinding.h"
#include "Sdp.h"

namespace
{
std::shared_ptr<RtspMediaSession> Apply(const std::string& text)
{
    auto parsed = sdp::Sdp::Parse(text);
    EXPECT_TRUE(parsed.ok) << parsed.message;
    if (!parsed.ok)
    {
        return nullptr;
    }

    auto session = RtspMediaSession::CreateNew("live/test");
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

TEST(RtpStreamContext, KeepsLegacyCandidatesInMediaOrder)
{
    sdp::SdpSession sdp;
    sdp::SdpMedia audio;
    audio.media = "audio";
    audio.fmts = {"128", "8", "0"};
    sdp.medias.push_back(audio);
    const auto context = media::RtpStreamContextBuilder::BuildFromSdp(sdp);
    ASSERT_EQ(context->tracks.size(), 1U);
    const auto& track = context->tracks.front();
    EXPECT_EQ(track.media_index, 0);
    EXPECT_EQ(track.media_type, media::MediaType::Audio);
    ASSERT_EQ(track.payloads.size(), 2U);
    EXPECT_EQ(track.payloads[0].payload_type, 8);
    EXPECT_EQ(track.payloads[0].codec, media::CodecType::PCMA);
    EXPECT_EQ(track.payloads[0].clock_rate, 8000);
    EXPECT_EQ(track.payloads[1].payload_type, 0);
    EXPECT_EQ(track.payloads[1].codec, media::CodecType::PCMU);
}

TEST(RtpStreamContext, UsesStructuredCodecsAndSharedMediaTypes)
{
    sdp::SdpSession sdp;
    sdp::SdpMedia video;
    video.media = "video";
    video.fmts = {"96"};
    video.codecs.push_back({102, "H264", 90000, 0, "packetization-mode=1", {}});
    sdp.medias.push_back(video);
    const auto context = media::RtpStreamContextBuilder::BuildFromSdp(sdp);
    ASSERT_EQ(context->tracks.size(), 1U);
    const auto& track = context->tracks.front();
    EXPECT_EQ(track.media_type, media::MediaType::Video);
    ASSERT_EQ(track.payloads.size(), 1U);
    EXPECT_EQ(track.payloads[0].payload_type, 102);
    EXPECT_EQ(track.payloads[0].codec, media::CodecType::H264);
    EXPECT_EQ(track.payloads[0].clock_rate, 90000);
    EXPECT_EQ(track.payloads[0].channels, 0);
    EXPECT_EQ(track.payloads[0].fmtp, "packetization-mode=1");
}

TEST(SdpTrackBinding, SelectsPayloadInMediaOrderAndMatchingFmtp)
{
    auto session = Apply(std::string(kSessionHeader) +
        "m=audio 0 RTP/AVP 111 0\r\n"
        "a=rtpmap:0 PCMU/8000\r\n"
        "a=rtpmap:111 opus/48000/2\r\n"
        "a=fmtp:0 ignored=1\r\n"
        "a=fmtp:111 minptime=10\r\n"
        "a=extmap:3 http://www.ietf.org/id/draft-holmer-rmcat-transport-wide-cc-extensions-01\r\n"
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
    EXPECT_EQ(info.transport_cc_extension_id, 3);
    media::MediaStreamTrack track;
    ASSERT_TRUE(session->GetMediaStream()->GetTrack(0, &track));
    EXPECT_EQ(track.codec, media::CodecType::Opus);
    EXPECT_EQ(track.sample_rate, 48000U);
    EXPECT_EQ(track.channels, 2);
}

TEST(SdpTrackBinding, AacAliasesAgreeAcrossSnapshotCoreAndLegacyReceiver)
{
    for (const auto* name : {"AAC", "aAc", "MPEG4-GENERIC"})
    {
        auto session = Apply(std::string(kSessionHeader) +
            "m=audio 0 RTP/AVP 96\r\na=rtpmap:96 " + name + "/44100/2\r\n"
            "a=fmtp:96 config=1210;mode=AAC-hbr\r\na=control:audio\r\n");
        ASSERT_TRUE(session);
        media::RtpPayloadInfo payload;
        ASSERT_TRUE(session->FindPayloadType(0, 96, &payload));
        EXPECT_EQ(payload.codec, media::CodecType::AAC);
        TrackInfo info;
        ASSERT_TRUE(session->GetTrackInfo(0, &info));
        EXPECT_EQ(info.codec_id, CodecId::AAC);
        EXPECT_EQ(info.codec_name, name);
        EXPECT_EQ(info.clock_rate, 44100U);
        EXPECT_EQ(info.fmtp, "config=1210;mode=AAC-hbr");
        media::MediaStreamTrack track;
        ASSERT_TRUE(session->GetMediaStream()->GetTrack(0, &track));
        EXPECT_EQ(track.codec, payload.codec);
        EXPECT_EQ(track.sample_rate, 44100U);
        // 接收器得到独立的兼容参数，修改返回值不改变协商快照。
        info.payload_type = 120;
        info.fmtp.clear();
        TrackInfo again;
        ASSERT_TRUE(session->GetTrackInfo(0, &again));
        EXPECT_EQ(again.payload_type, 96);
        EXPECT_EQ(again.fmtp, "config=1210;mode=AAC-hbr");
    }
}

TEST(SdpTrackBinding, VideoRtpParametersSurviveRepublishWithoutEnteringCoreDescription)
{
    auto session = Apply(std::string(kSessionHeader) +
        "m=video 0 RTP/AVP 102\r\n"
        "a=rtpmap:102 H264/90000\r\n"
        "a=fmtp:102 packetization-mode=1;profile-level-id=42e01f\r\n"
        "a=extmap:4 http://www.ietf.org/id/draft-holmer-rmcat-transport-wide-cc-extensions-01\r\n"
        "a=control:rtsp://camera/live/test/video\r\n");
    ASSERT_TRUE(session);
    ASSERT_TRUE(session->Register());
    session->Close();
    auto next = session->CreateNextPublication();
    ASSERT_TRUE(next);
    EXPECT_NE(next->GetId(), session->GetId());
    ASSERT_TRUE(next->Register());
    media::MediaStreamTrack track;
    ASSERT_TRUE(next->GetMediaStream()->GetTrack(0, &track));
    EXPECT_EQ(track.type, media::MediaType::Video);
    EXPECT_EQ(track.codec, media::CodecType::H264);
    EXPECT_EQ(track.sample_rate, 0U);
    EXPECT_EQ(track.channels, 0);
    TrackInfo info;
    ASSERT_TRUE(next->GetTrackInfo(0, &info));
    EXPECT_EQ(info.payload_type, 102);
    EXPECT_EQ(info.clock_rate, 90000U);
    EXPECT_EQ(info.codec_id, CodecId::H264);
    EXPECT_EQ(info.codec_name, "H264");
    EXPECT_EQ(info.fmtp, "packetization-mode=1;profile-level-id=42e01f");
    EXPECT_EQ(info.transport_cc_extension_id, 4);
    EXPECT_EQ(info.control, "video");
    EXPECT_TRUE(next->GetTrackDescription("rtsp://camera/live/test/video"));
    media::RtpPayloadInfo payload;
    ASSERT_TRUE(next->FindPayloadType(0, 102, &payload));
    EXPECT_EQ(payload.codec, media::CodecType::H264);
    EXPECT_FALSE(next->GetTrackInfo(-1, &info));
    EXPECT_FALSE(next->GetTrackInfo(1, &info));
    EXPECT_FALSE(next->GetTrackInfo(0, nullptr));
}

TEST(SdpTrackBinding, BoundTrackReplacementPreservesSelectedRtpParameters)
{
    auto session = Apply(std::string(kSessionHeader) +
        "m=audio 0 RTP/AVP 0\r\na=control:audio\r\n");
    ASSERT_TRUE(session);
    ASSERT_TRUE(session->BindTrackEndpoint(0, 99));
    auto replacement = sdp::Sdp::Parse(std::string(kSessionHeader) +
        "m=audio 0 RTP/AVP 8\r\na=control:replacement\r\n");
    ASSERT_TRUE(replacement.ok);
    std::string error;
    EXPECT_FALSE(session->ApplySdp(replacement.session, &error));
    TrackInfo info;
    ASSERT_TRUE(session->GetTrackInfo(0, &info));
    EXPECT_EQ(info.payload_type, 0);
    EXPECT_EQ(info.codec_id, CodecId::PCMU);
    EXPECT_EQ(info.clock_rate, 8000U);
    EXPECT_EQ(info.control, "audio");
    EXPECT_TRUE(session->GetTrackDescription("audio"));
    EXPECT_FALSE(session->GetTrackDescription("replacement"));
    media::RtpPayloadInfo payload;
    EXPECT_TRUE(session->FindPayloadType(0, 0, &payload));
    EXPECT_FALSE(session->FindPayloadType(0, 8, &payload));
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

    EXPECT_NE(session->GetTrackDescription("trackID=audio"), nullptr);
    EXPECT_NE(session->GetTrackDescription("rtsp://camera/live/test/trackID=audio"), nullptr);
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

    media::RtpPayloadInfo audio;
    media::RtpPayloadInfo video;
    ASSERT_TRUE(session->FindPayloadType(0, 96, &audio));
    ASSERT_TRUE(session->FindPayloadType(1, 96, &video));
    EXPECT_EQ(audio.codec, media::CodecType::Opus);
    EXPECT_EQ(audio.clock_rate, 48000);
    EXPECT_EQ(video.codec, media::CodecType::H264);
    EXPECT_EQ(video.clock_rate, 90000);
}

TEST(SdpTrackBinding, FailedReplacementPreservesCoreDescriptionAndProtocolMapping)
{
    auto session = Apply(std::string(kSessionHeader) +
        "m=audio 0 RTP/AVP 0\r\na=control:audio\r\n");
    ASSERT_TRUE(session);
    auto invalid = sdp::Sdp::Parse(std::string(kSessionHeader) +
        "m=audio 0 RTP/AVP 8\r\na=control:duplicate\r\n"
        "m=video 0 RTP/AVP 96\r\na=rtpmap:96 H264/90000\r\na=control:duplicate\r\n");
    ASSERT_TRUE(invalid.ok);
    std::string error;
    EXPECT_FALSE(session->ApplySdp(invalid.session, &error));
    EXPECT_FALSE(error.empty());
    EXPECT_TRUE(session->GetTrackDescription("audio"));
    EXPECT_FALSE(session->GetTrackDescription("duplicate"));
    const auto tracks = session->GetMediaStream()->GetTracks();
    ASSERT_EQ(tracks.size(), 1U);
    EXPECT_EQ(tracks[0].codec, media::CodecType::PCMU);
}
