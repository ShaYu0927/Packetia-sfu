#include "RtpSenderBinding.h"
#include "Sdp.h"
#include <iostream>
#include <stdexcept>

#define CHECK(value) do { if (!(value)) throw std::runtime_error( \
    std::string(__func__) + ":" + std::to_string(__LINE__) + ": " + #value); } while (false)

namespace
{
constexpr const char* kMid = "urn:ietf:params:rtp-hdrext:sdes:mid";
constexpr const char* kTwcc = "http://www.ietf.org/id/draft-holmer-rmcat-transport-wide-cc-extensions-01";

sdp::SdpMedia Video(int pt = 102, uint32_t ssrc = 11)
{
    sdp::SdpMedia section;
    section.media = "video";
    section.mid = "v";
    section.direction = sdp::MediaDirection::RecvOnly;
    section.codecs = {sdp::SdpCodec::H264(pt)};
    section.codecs.front().rtcpFeedback = {{"nack"}, {"nack", "pli"}};
    section.headerExtensions = {{1, kMid}, {3, kTwcc}};
    section.ssrcs = {{ssrc, {{"cname", "camera"}}}};
    section.rtcpMux = true;
    return section;
}

void RtxAssociationAndPrimarySelection()
{
    auto section = Video();
    const auto rtx = sdp::SdpCodec::Rtx(103, section.codecs.front(), 250);
    section.codecs.insert(section.codecs.begin(), rtx); // Repair codec can precede primary.
    section.ssrcs.insert(section.ssrcs.begin(), {22, {}});
    section.ssrcGroups = {{"FID", {11, 22}}};
    section.rtcpFeedback = {{"nack"}, {"nack", "pli"}};
    media::RtpParameters params;
    std::string error = "old error";
    CHECK(media::BuildRtpParameters(section, params, &error));
    CHECK(error.empty());
    CHECK(params.codec.payloadType == 102);
    CHECK(params.rtx && params.rtx->payloadType == 103);
    CHECK(sdp::SdpCodec::AssociatedPayloadType(*params.rtx) == 102);
    CHECK(params.rtx->fmtp.find("rtx-time=250") != std::string::npos);
    CHECK(params.encodings.size() == 1 && params.encodings.front().ssrc == 11);
    CHECK(params.encodings.front().rtx_ssrc == 22 && params.encodings.front().cname == "camera");
    CHECK(params.codec.rtcpFeedback.size() >= 2 && params.rtcp_mux);
    const auto receiver = media::ToRtpTrackInfo(params, 7);
    CHECK(receiver.track_index == 7 && receiver.payload_type == 102 && receiver.codec_id == CodecId::H264);
    CHECK(receiver.transport_cc_extension_id == 3);
    section.codecs.front().fmtp = "apt=99";
    CHECK(!media::BuildRtpParameters(section, params, &error));
    CHECK(!error.empty() && params.codec.payloadType == 102 && params.encodings.front().rtx_ssrc == 22);
    section.codecs.front() = rtx;
    section.ssrcGroups.front().ssrcs = {11, 11};
    CHECK(!media::BuildRtpParameters(section, params));
    section.ssrcGroups.front().ssrcs = {11, 22};
    section.headerExtensions.push_back({1, "duplicate-id"});
    CHECK(!media::BuildRtpParameters(section, params));
}

void SubscriberUsesItsOwnWireParameters()
{
    media::RtpParameters receive, send;
    CHECK(media::BuildRtpParameters(Video(), receive));
    auto downstream = Video(120, 777);
    downstream.mid = "subscriber-v";
    downstream.direction = sdp::MediaDirection::SendOnly;
    downstream.headerExtensions = {{4, kMid, sdp::MediaDirection::SendOnly}, {6, kTwcc, sdp::MediaDirection::RecvOnly}};
    CHECK(media::BuildRtpParameters(downstream, send));
    rtsp::RtpSenderTrackConfig config;
    config.rtp_cache_size = 1024;
    CHECK(media::BindRtpSenderParameters(send, config));
    CHECK(config.payload_type == 120 && config.local_ssrc == 777 && config.sample_rate == 90000);
    CHECK(config.mid_extension_id == 4 && config.mid == "subscriber-v" && config.transport_cc_extension_id == 0);
    CHECK(config.rewrite_payload_type && config.rewrite_header_extensions && config.rtp_cache_size == 1024);
    CHECK(receive.codec.payloadType == 102 && receive.encodings.front().ssrc == 11);
    CHECK(!media::BindRtpSenderParameters(receive, config));
    CHECK(config.payload_type == 120 && config.local_ssrc == 777); // Failed bind is atomic.
    send.rtx = sdp::SdpCodec::Rtx(121, send.codec);
    CHECK(!media::BindRtpSenderParameters(send, config)); // Sending RTX also requires a repair SSRC.
    send.encodings.front().rtx_ssrc = 778;
    CHECK(media::BindRtpSenderParameters(send, config));
    CHECK(config.rtx_payload_type == 121 && config.rtx_ssrc == 778);
    send.rtx->payloadType = -1;
    CHECK(!media::BindRtpSenderParameters(send, config));
    CHECK(config.rtx_payload_type == 121); // Malformed direct parameters cannot alter the sender.
    send.rtx->payloadType = 120;
    CHECK(!media::BindRtpSenderParameters(send, config));
    send.rtx->payloadType = 121;
    send.rtx->clockRate = 48000;
    CHECK(!media::BindRtpSenderParameters(send, config));
    send.rtx.reset();
    send.encodings.front().rtx_ssrc.reset();
    send.header_extensions.front().id = 17;
    CHECK(!media::BindRtpSenderParameters(send, config)); // Current writer is one-byte only.
    CHECK(config.mid_extension_id == 4);
    downstream.headerExtensions.back().direction = sdp::MediaDirection::SendOnly;
    CHECK(media::BuildRtpParameters(downstream, send));
    CHECK(media::BindRtpSenderParameters(send, config) && config.transport_cc_extension_id == 6);
    send.codec.rtcpFeedback.clear();
    CHECK(media::BindRtpSenderParameters(send, config));
    CHECK(!config.nack_enabled && !config.pli_enabled && !config.fir_enabled && config.rtp_cache_size == 0);
    CHECK(media::ToRtpTrackInfo(send, 0).nack_enabled == false);
}

void LegacySdpAndExtensionDirections()
{
    const auto parsed = sdp::Sdp::Parse("v=0\r\no=- 1 1 IN IP4 127.0.0.1\r\ns=test\r\nt=0 0\r\n"
        "m=audio 0 RTP/AVP 111 0\r\na=rtpmap:0 PCMU/8000\r\na=rtpmap:111 opus/48000/2\r\n"
        "a=fmtp:111 minptime=10\r\na=control:trackID=audio\r\na=extmap:5/recvonly " + std::string(kTwcc) + "\r\n");
    CHECK(parsed.ok);
    ::TrackInfo legacy;
    CHECK(media::BuildRtpTrackInfo(parsed.session.medias.front(), 2, legacy));
    CHECK(legacy.payload_type == 111 && legacy.codec_id == CodecId::OPUS && legacy.channels == 2);
    CHECK(legacy.fmtp == "minptime=10" && legacy.control == "trackID=audio" && legacy.transport_cc_extension_id == 5);
    sdp::SdpMedia pcmu;
    pcmu.media = "audio";
    pcmu.fmts = {"0"};
    CHECK(media::BuildRtpTrackInfo(pcmu, 3, legacy));
    CHECK(legacy.payload_type == 0 && legacy.clock_rate == 8000 && legacy.channels == 1);
    auto section = Video();
    section.headerExtensions.back().direction = sdp::MediaDirection::SendOnly;
    media::RtpParameters params;
    CHECK(media::BuildRtpParameters(section, params));
    CHECK(params.ExtensionId(kTwcc, media::RtpFlow::Receive) == 0);
    CHECK(media::ToRtpTrackInfo(params, 0).transport_cc_extension_id == 0);
    section.headerExtensions.back().direction = sdp::MediaDirection::RecvOnly;
    CHECK(media::BuildRtpParameters(section, params));
    CHECK(params.ExtensionId(kTwcc, media::RtpFlow::Receive) == 3);
    section.direction = sdp::MediaDirection::Inactive;
    CHECK(media::BuildRtpParameters(section, params));
    CHECK(!params.Allows(media::RtpFlow::Send) && !params.Allows(media::RtpFlow::Receive));
}
}

int main()
{
    try {
        RtxAssociationAndPrimarySelection();
        SubscriberUsesItsOwnWireParameters();
        LegacySdpAndExtensionDirections();
        std::cout << "RTP parameter binding tests passed\n";
        return 0;
    } catch (const std::exception& e) { std::cerr << e.what() << '\n'; return 1; }
}
