#include "Sdp.h"
#include "SdpUtil.h"

#include <iostream>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace
{
using namespace sdp;

void Check(bool condition, const char* expression, int line)
{
    if (!condition) throw std::runtime_error(std::to_string(line) + ": " + expression);
}
#define CHECK(value) Check(static_cast<bool>(value), #value, __LINE__)

std::string Digest(std::size_t bytes, const std::string& octet = "ab")
{
    std::string result;
    for (std::size_t i = 0; i < bytes; ++i)
    {
        if (i != 0) result += ':';
        result += octet;
    }
    return result;
}

std::string Header()
{
    return "v=0\r\no=- 18446744073709551615 0 IN IP4 127.0.0.1\r\n"
        "s=parser test\r\nt=0 0\r\na=group:BUNDLE audio\r\n"
        "a=ice-ufrag:sessionUser\r\na=ice-pwd:sessionPassword\r\na=setup:actpass\r\n"
        "a=fingerprint:sha-256 " + Digest(32) + "\r\n";
}

std::string Audio()
{
    return "m=audio 9 UDP/TLS/RTP/SAVPF 111 0\r\nc=IN IP4 0.0.0.0\r\n"
        "a=mid:audio\r\na=rtcp-mux\r\na=rtcp-rsize\r\na=rtpmap:111 opus/48000/2\r\n"
        "a=fmtp:111 useinbandfec=1\r\na=rtcp-fb:* transport-cc\r\na=rtcp-fb:111 nack\r\n";
}

std::string Replace(std::string text, const std::string& before, const std::string& after)
{
    const auto position = text.find(before);
    CHECK(position != std::string::npos);
    text.replace(position, before.size(), after);
    return text;
}

SdpSession Parse(const std::string& text)
{
    SdpSession result;
    std::string error = "previous error";
    CHECK(Sdp::Parse(text, SdpProfile::WebRtc, SdpType::Offer, result, error));
    CHECK(error.empty());
    return result;
}

void Reject(const std::string& text)
{
    SdpSession result;
    result.type = SdpType::Answer;
    result.ice.ufrag = "unchanged";
    result.medias.resize(1);
    result.medias.front().mid = "sentinel";
    std::string error;
    if (Sdp::Parse(text, SdpProfile::WebRtc, SdpType::Offer, result, error))
        throw std::runtime_error("Accepted invalid SDP: " + text);
    CHECK(!error.empty());
    CHECK(result.type == SdpType::Answer && result.ice.ufrag == "unchanged");
    CHECK(result.medias.size() == 1 && result.medias.front().mid == "sentinel");
}

void RawRtspRoundTrip()
{
    const std::string text = "v=0\r\no=- 1 2 IN IP4 127.0.0.1\r\ns=camera\r\n"
        "c=IN IP4 239.1.2.3/127\r\nt=0 0\r\na=control:*\r\n"
        "m=video 5004/2 RTP/AVP 96\r\na=rtpmap:96 H264/90000\r\n"
        "a=fmtp:96 packetization-mode=1;profile-level-id=42e01f\r\n"
        "a=control:trackID=1\r\na=x-camera:opaque:value\r\n";
    const auto parsed = sdp::Sdp::Parse(text);
    CHECK(parsed.ok && parsed.session.medias.front().portCount == 2);
    CHECK(parsed.session.connection == "IN IP4 239.1.2.3/127");
    CHECK(parsed.session.medias.front().GetAttribute("control") == "trackID=1");
    const auto serialized = sdp::Sdp::Serialize(parsed.session);
    CHECK(serialized == text);
    CHECK(sdp::Sdp::Serialize(sdp::Sdp::Parse(serialized).session) == serialized);

    // Numeric payload aliases must not cause synthesized duplicate rtpmap/fmtp.
    auto padded = Replace(Replace(text, "a=rtpmap:96", "a=rtpmap:096"), "a=fmtp:96", "a=fmtp:096");
    auto roundtrip = sdp::Sdp::Parse(sdp::Sdp::Serialize(sdp::Sdp::Parse(padded).session));
    CHECK(roundtrip.ok && roundtrip.session.medias.front().rtpmaps.size() == 1);
    CHECK(roundtrip.session.medias.front().fmtps.size() == 1);
}

void RawSyntaxAndNumbers()
{
    const auto valid = Header() + Audio();
    const std::vector<std::string> invalid = {
        "x", "vx0\n", "a\n", "v0\n", "v=0\nv=0\n",
        Replace(valid, "v=0", "v=0trailing"),
        Replace(valid, "18446744073709551615", "18446744073709551616"),
        Replace(valid, "t=0 0", "t=0 -1"),
        Replace(valid, "t=0 0", "t=0 1trailing"),
        Replace(valid, "m=audio 9 ", "m=audio 65536 "),
        Replace(valid, "m=audio 9 ", "m=audio 9trailing "),
        Replace(valid, "m=audio 9 ", "m=audio 9/0 "),
        Replace(valid, "m=audio 9 ", "m=audio 9/2trailing "),
        Replace(valid, "opus/48000/2", "opus/48000/2/3"),
        Replace(valid, "opus/48000/2", "opus/48000/2trailing"),
        Replace(valid, "opus/48000/2", "opus/0/2"),
        Replace(valid, "a=rtpmap:111", "a=rtpmap:128"),
        Replace(valid, "a=fmtp:111", "a=fmtp:111trailing"),
        valid + "a=x-unknown:" + std::string(1, '\0') + "\r\n",
        valid + "a=x-unknown:\v\r\n",
        valid + "a=x-unknown:\x7f\r\n",
        valid + "a=x-unknown:embedded\rvalue\r\n",
    };
    for (const auto& text : invalid)
    {
        sdp::SdpSession preserved;
        preserved.session_name = "sentinel";
        std::string error;
        CHECK(!sdp::SdpParser::Parse(text, preserved, error));
        CHECK(!error.empty() && preserved.session_name == "sentinel");
    }
}

void FingerprintAndExtensionBounds()
{
    const auto valid = Header() + Audio();
    for (const auto& algorithm : std::vector<std::pair<std::string, std::size_t>>{
        {"sha-1", 20}, {"sha-224", 28}, {"sha-256", 32}, {"sha-384", 48}, {"sha-512", 64}})
    {
        const auto text = Replace(valid, "sha-256 " + Digest(32), algorithm.first + " " + Digest(algorithm.second));
        const auto parsed = Parse(text);
        CHECK(parsed.dtls.fingerprints.front().value == Digest(algorithm.second, "AB"));
        Reject(Replace(text, algorithm.first + " " + Digest(algorithm.second), algorithm.first + " AA"));
    }
    Reject(Replace(valid, "sha-256", "sha-999"));
    Reject(Replace(valid, Digest(32), Digest(32, "G0")));
    CHECK(Parse(valid + "a=extmap:255/recvonly urn:example:extension attributes\r\n")
        .medias.front().headerExtensions.front().id == 255);
    Reject(valid + "a=extmap:256 urn:example:extension\r\n");
    Reject(valid + "a=extmap:0 urn:example:extension\r\n");
    Reject(valid + "a=extmap:1trailing urn:example:extension\r\n");
    Reject(valid + "a=extmap:1/invalid urn:example:extension\r\n");
    Reject(valid + "a=extmap:1 urn:ietf:params:rtp-hdrext:encrypt urn:example:extension\r\n");
}

void CriticalDuplicatesAndMalformedIntegers()
{
    const auto valid = Header() + Audio();
    for (const auto& suffix : std::vector<std::string>{
        "a=mid:audio\r\n", "a=rtcp-mux\r\n", "a=rtpmap:111 opus/48000/2\r\n",
        "a=fmtp:111 useinbandfec=1\r\n", "a=sendonly\r\na=recvonly\r\n",
        "a=ice-ufrag:one\r\na=ice-ufrag:two\r\n", "a=setup:active\r\na=setup:passive\r\n",
        "a=extmap:1 urn:first\r\na=extmap:1 urn:second\r\n",
        "a=ssrc:4294967296 cname:overflows\r\n", "a=ssrc:10x cname:bad\r\n",
        "a=ssrc-group:FID 1 2x\r\n", "a=rtcp-fb:111x nack\r\n",
        "a=rtcp:9x\r\n", "a=sctp-port:65536\r\n", "a=max-message-size:1x\r\n",
        "a=candidate:1 1 UDP 2122260223 127.0.0.1 9x typ host\r\n",
        "a=candidate:1 1 UDP 4294967296 127.0.0.1 9 typ host\r\n"})
        Reject(valid + suffix);
    Reject(Replace(valid, "m=audio 9 ", "m=audio 9/2 "));
    Reject(Replace(valid, "IN IP4 127.0.0.1", "IN INVALID 127.0.0.1"));
    Reject(Replace(valid, "111 0\r\n", "111 111\r\n"));
}

void ExtractedMediaAndRoundTrip()
{
    const auto text = Header() + Audio()
        + "a=ice-ufrag:mediaUser\r\na=recvonly\r\na=msid:stream track\r\n"
          "a=ssrc:42 cname:one\r\na=ssrc:42 msid:stream track\r\n"
          "a=ssrc:43 cname:two\r\na=ssrc-group:FID 42 43\r\n"
          "a=extmap:3/sendonly urn:example:extension attributes\r\n"
          "a=candidate:1 1 UDP 2122260223 127.0.0.1 5000 typ host generation 0\r\n"
          "a=end-of-candidates\r\na=x-unknown:preserved:value\r\n";
    const auto parsed = Parse(text);
    const auto& media = parsed.medias.front();
    CHECK(media.ice.ufrag == "mediaUser" && media.ice.pwd == "sessionPassword");
    CHECK(media.direction == MediaDirection::RecvOnly && media.rtcpMux && media.rtcpRsize);
    CHECK(media.codecs.size() == 2 && media.codecs[0].payloadType == 111 && media.codecs[1].payloadType == 0);
    CHECK(media.codecs[1].encodingName == "PCMU");
    CHECK(media.rtcpFeedback.size() == 1 && media.codecs[0].rtcpFeedback.size() == 1);
    CHECK(media.ssrcs.size() == 2 && media.ssrcs[0].attributes.size() == 2);
    CHECK(media.ssrcGroups.front().ssrcs == std::vector<uint32_t>({42, 43}));
    CHECK(media.headerExtensions.front().direction == MediaDirection::SendOnly);
    CHECK(media.msids == std::vector<std::string>({"stream track"}));
    CHECK(media.ice.endOfCandidates && media.ice.candidates.size() == 1);
    const auto serialized = Sdp::Serialize(parsed);
    CHECK(serialized.find("a=x-unknown:preserved:value\r\n") != std::string::npos);
    CHECK(Sdp::Serialize(Parse(serialized)) == serialized);
}

void SharedApiProfilesAndTypedEdits()
{
    // RTSP keeps its existing raw-attribute API and does not require MID or mux.
    const std::string rtsp = "v=0\r\no=- 1 0 IN IP4 127.0.0.1\r\ns=camera\r\nt=0 0\r\n"
        "m=video 5004/2 RTP/AVP 96\r\na=rtpmap:96 H264/90000\r\na=control:trackID=1\r\n";
    const auto generic = Sdp::Parse(rtsp);
    CHECK(generic.ok && generic.session.profile == SdpProfile::Generic);
    CHECK(generic.session.medias.front().rtpmaps.front().encodingName == "H264");
    CHECK(Sdp::Serialize(generic.session) == rtsp);
    CHECK(!Sdp::Parse(rtsp, SdpProfile::WebRtc).ok);

    auto parsed = Sdp::Parse(Header() + Audio()
        + "a=extmap:3 urn:example:old\r\na=ssrc:42 cname:old\r\n"
          "a=msid:oldStream oldTrack\r\na=x-vendor:opaque:value\r\n", SdpProfile::WebRtc);
    CHECK(parsed.ok && parsed.session.profile == SdpProfile::WebRtc);
    auto& session = parsed.session;
    auto& media = session.medias.front();
    CHECK(!media.HasAttribute("mid") && !media.HasAttribute("rtpmap"));
    CHECK(media.rtpmaps.empty() && media.fmtps.empty());
    media.mid = "renamed";
    session.bundle.mids = {"renamed"};
    media.direction = MediaDirection::SendOnly;
    media.ice.ufrag = "newUser";
    media.ice.pwd = "newPassword";
    media.dtls.setup = DtlsSetup::Passive;
    media.dtls.fingerprints = {{"sha-256", Digest(32, "CD")}};
    media.codecs.resize(1);
    media.codecs.front().payloadType = 112;
    media.codecs.front().fmtp = "useinbandfec=0";
    media.codecs.front().rtcpFeedback.clear();
    media.rtcpFeedback.clear();
    media.headerExtensions = {{4, "urn:example:new", MediaDirection::SendOnly, "extra"}};
    media.ssrcs = {{84, {{"cname", "new"}}}};
    media.msids = {"newStream newTrack"};
    media.rtcpRsize = false;
    const auto text = Sdp::Serialize(session);
    CHECK(text.find("a=x-vendor:opaque:value\r\n") != std::string::npos);
    CHECK(text.find("a=rtpmap:111") == std::string::npos);
    CHECK(text.find("a=rtcp-fb:") == std::string::npos);
    CHECK(text.find("a=rtcp-rsize") == std::string::npos);
    CHECK(text.find("a=msid:oldStream") == std::string::npos);
    const auto roundtrip = Parse(text);
    const auto& updated = roundtrip.medias.front();
    CHECK(roundtrip.bundle.mids == std::vector<std::string>{"renamed"});
    CHECK(updated.mid == "renamed" && updated.direction == MediaDirection::SendOnly);
    CHECK(updated.ice.ufrag == "newUser" && updated.ice.pwd == "newPassword");
    CHECK(updated.dtls.setup == DtlsSetup::Passive);
    CHECK(updated.dtls.fingerprints.front().value == Digest(32, "CD"));
    CHECK(updated.fmts == std::vector<std::string>{"112"});
    CHECK(updated.codecs.size() == 1 && updated.codecs.front().fmtp == "useinbandfec=0");
    CHECK(updated.headerExtensions.front().id == 4);
    CHECK(updated.headerExtensions.front().uri == "urn:example:new");
    CHECK(updated.ssrcs.front().ssrc == 84 && updated.msids.front() == "newStream newTrack");
    CHECK(Sdp::Serialize(roundtrip) == text);

    // There is only one media list: removals affect serialized output directly.
    session.medias.clear();
    session.bundle.mids.clear();
    CHECK(Sdp::Serialize(session).find("m=") == std::string::npos);
}

void SignalingTypesAndFailurePreserveOutput()
{
    SdpSession session;
    std::string error;
    for (const auto type : {SdpType::Offer, SdpType::Answer, SdpType::Pranswer})
    {
        CHECK(Sdp::Parse(Header() + Audio(), SdpProfile::WebRtc, type, session, error));
        CHECK(session.type == type && error.empty());
    }
    const auto before = Sdp::Serialize(session);
    CHECK(!Sdp::Parse(Header() + Audio(), SdpProfile::WebRtc, SdpType::Rollback, session, error));
    CHECK(!error.empty() && session.type == SdpType::Pranswer);
    CHECK(Sdp::Serialize(session) == before);
}
} // namespace

int main()
{
    try
    {
        RawRtspRoundTrip();
        RawSyntaxAndNumbers();
        FingerprintAndExtensionBounds();
        CriticalDuplicatesAndMalformedIntegers();
        ExtractedMediaAndRoundTrip();
        SharedApiProfilesAndTypedEdits();
        SignalingTypesAndFailurePreserveOutput();
        std::cout << "SDP parser and serializer boundary tests passed\n";
        return 0;
    }
    catch (const std::exception& error)
    {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
