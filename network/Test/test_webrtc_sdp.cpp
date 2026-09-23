#include "Sdp.h"
#include "WebRtcSession.h"

#include <algorithm>
#include <cctype>
#include <functional>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

using namespace protocol::webrtc;

namespace
{
#define CHECK(condition) do { if (!(condition)) throw std::runtime_error( \
    std::string(__func__) + ":" + std::to_string(__LINE__) + ": " #condition); } while (false)

const std::string kRemoteFingerprint =
    "11:11:11:11:11:11:11:11:11:11:11:11:11:11:11:11:"
    "11:11:11:11:11:11:11:11:11:11:11:11:11:11:11:11";
const std::string kLocalFingerprint =
    "22:22:22:22:22:22:22:22:22:22:22:22:22:22:22:22:"
    "22:22:22:22:22:22:22:22:22:22:22:22:22:22:22:22";
const std::string kRemotePassword = "remotePassword0123456789";
const std::string kLocalPassword = "localPassword01234567890";

// SDP negotiation needs a certificate identity, but must not require a socket
// or initiate a DTLS/SRTP handshake. These backends deliberately cannot do so.
class IdentityOnlyDtls final : public DtlsTransport
{
public:
    DtlsParameters LocalParameters() const override
    {
        DtlsParameters result;
        result.fingerprints.push_back({"sha-256", kLocalFingerprint});
        return result;
    }
    bool Configure(const DtlsParameters&, DtlsSetup) override { return false; }
    bool Start(SendCallback) override { return false; }
    bool HandleDatagram(const uint8_t*, size_t) override { return false; }
    bool Tick(uint64_t) override { return false; }
    bool IsConnected() const noexcept override { return false; }
    bool ExportSrtpKeys(SrtpKeyingMaterial&) const override { return false; }
    void Close() noexcept override {}
};

class UnusedSrtp final : public SrtpTransport
{
public:
    bool Configure(const SrtpKeyingMaterial&) override { return false; }
    bool UnprotectRtp(std::vector<uint8_t>&) override { return false; }
    bool UnprotectRtcp(std::vector<uint8_t>&) override { return false; }
    bool ProtectRtp(std::vector<uint8_t>&) override { return false; }
    bool ProtectRtcp(std::vector<uint8_t>&) override { return false; }
    void Close() noexcept override {}
};

WebRtcSessionOptions Options()
{
    WebRtcSessionOptions options;
    options.ice.ufrag = "local";
    options.ice.pwd = kLocalPassword;
    options.ice.candidates = {"1 1 udp 2130706431 192.0.2.1 5000 typ host"};
    options.ice.endOfCandidates = true;
    options.origin.sess_id = "123456789";

    WebRtcMediaDescription video;
    video.media = "video";
    video.direction = MediaDirection::SendRecv;
    video.rtcpMux = true;
    video.rtcpRsize = true;
    video.codecs.push_back({96, "H264", 90000, 0,
        "profile-level-id=42e01f;packetization-mode=1;level-asymmetry-allowed=1",
        {{"nack", ""}, {"nack", "pli"}}});
    video.rtcpFeedback.push_back({"transport-cc", ""});
    video.headerExtensions.push_back({1, RtpHeaderExtensionUri::SDES_MID,
                                      MediaDirection::SendRecv, ""});
    video.headerExtensions.push_back({2, RtpHeaderExtensionUri::TRANSPORT_CC,
                                      MediaDirection::SendRecv, ""});
    video.ssrcs.push_back({222222, {{"cname", "localCname"}}});
    video.msids.push_back("localStream localVideo");
    options.medias.push_back(video);

    WebRtcMediaDescription audio;
    audio.media = "audio";
    audio.direction = MediaDirection::SendRecv;
    audio.rtcpMux = true;
    audio.codecs.push_back({111, "opus", 48000, 2,
        "minptime=10;useinbandfec=1;stereo=0", {{"nack", ""}}});
    options.medias.push_back(audio);
    return options;
}

std::shared_ptr<WebRtcSession> Session(WebRtcSessionOptions options = Options())
{
    return std::make_shared<WebRtcSession>(nullptr, std::make_unique<IdentityOnlyDtls>(),
        std::make_unique<UnusedSrtp>(), nullptr, std::move(options));
}

std::string Header(const std::string& mids = "video", const std::string& direction = "recvonly")
{
    return "v=0\r\n"
           "o=- 987654321 2 IN IP4 127.0.0.1\r\n"
           "s=-\r\n"
           "t=0 0\r\n"
           "a=group:BUNDLE " + mids + "\r\n"
           "a=msid-semantic: WMS remoteStream\r\n"
           "a=ice-ufrag:remote\r\n"
           "a=ice-pwd:" + kRemotePassword + "\r\n"
           "a=fingerprint:sha-256 " + kRemoteFingerprint + "\r\n"
           "a=setup:actpass\r\n"
           "a=" + direction + "\r\n";
}

std::string Video(const std::string& mid = "video",
                  const std::string& fmtp =
                      "level-asymmetry-allowed=1;packetization-mode=1;profile-level-id=42e01f")
{
    return "m=video 9 UDP/TLS/RTP/SAVPF 102 103 96\r\n"
           "c=IN IP4 0.0.0.0\r\n"
           "a=mid:" + mid + "\r\n"
           "a=rtcp-mux\r\n"
           "a=rtcp-rsize\r\n"
           "a=rtpmap:102 H264/90000\r\n"
           "a=fmtp:102 " + fmtp + "\r\n"
           "a=rtcp-fb:102 nack\r\n"
           "a=rtcp-fb:102 nack pli\r\n"
           "a=rtcp-fb:102 ccm fir\r\n"
           "a=rtcp-fb:* transport-cc\r\n"
           "a=rtpmap:103 rtx/90000\r\n"
           "a=fmtp:103 apt=102\r\n"
           "a=rtpmap:96 VP8/90000\r\n"
           "a=extmap:3/recvonly urn:ietf:params:rtp-hdrext:sdes:mid\r\n"
           "a=extmap:5/recvonly http://www.ietf.org/id/draft-holmer-rmcat-transport-wide-cc-extensions-01\r\n"
           "a=extmap:9 http://example.test/unsupported-extension\r\n"
           "a=ssrc:111111 cname:remoteCname\r\n"
           "a=ssrc:111111 msid:remoteStream remoteVideo\r\n"
           "a=ssrc:111112 cname:remoteCname\r\n"
           "a=ssrc-group:FID 111111 111112\r\n"
           "a=msid:remoteStream remoteVideo\r\n"
           "a=candidate:9 1 udp 2122260223 198.51.100.10 6000 typ host\r\n"
           "a=end-of-candidates\r\n";
}

std::string Audio(const std::string& mid = "audio")
{
    return "m=audio 9 UDP/TLS/RTP/SAVPF 111\r\n"
           "c=IN IP4 0.0.0.0\r\n"
           "a=mid:" + mid + "\r\n"
           "a=rtcp-mux\r\n"
           "a=rtpmap:111 opus/48000/2\r\n"
           "a=fmtp:111 minptime=20;useinbandfec=0;stereo=1\r\n"
           "a=rtcp-fb:111 nack\r\n";
}

std::string Replace(std::string input, const std::string& old_text, const std::string& new_text)
{
    const auto position = input.find(old_text);
    CHECK(position != std::string::npos);
    input.replace(position, old_text.size(), new_text);
    return input;
}

WebRtcSessionDescription Parse(const std::string& text, SdpType type = SdpType::Offer)
{
    WebRtcSessionDescription result;
    std::string error;
    if (!sdp::Sdp::Parse(text, sdp::SdpProfile::WebRtc, type, result, error))
        throw std::runtime_error("SDP parse failed: " + error);
    CHECK(error.empty());
    return result;
}

WebRtcSessionDescription Answer(const std::string& offer,
                               WebRtcSessionOptions options = Options())
{
    auto session = Session(std::move(options));
    if (!session->ApplyRemoteOffer(offer))
        throw std::runtime_error("Offer rejected: " + session->LastError());
    std::string answer;
    if (!session->CreateLocalAnswer(answer))
        throw std::runtime_error("Answer failed: " + session->LastError());
    CHECK(session->State() == WebRtcSessionState::HaveAnswer);
    CHECK(!answer.empty());
    CHECK(answer.find("v=0\r\n") == 0);
    return Parse(answer, SdpType::Answer);
}

std::string FmtpParameter(const std::string& fmtp, const std::string& name)
{
    size_t start = 0;
    while (start < fmtp.size())
    {
        const auto end = fmtp.find(';', start);
        auto parameter = fmtp.substr(start, end == std::string::npos ? end : end - start);
        parameter.erase(std::remove_if(parameter.begin(), parameter.end(),
            [](unsigned char c) { return std::isspace(c); }), parameter.end());
        std::transform(parameter.begin(), parameter.end(), parameter.begin(),
            [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
        if (parameter.compare(0, name.size() + 1, name + "=") == 0)
            return parameter.substr(name.size() + 1);
        if (end == std::string::npos) break;
        start = end + 1;
    }
    return {};
}

bool HasFeedback(const std::vector<RtcpFeedback>& feedback,
                 const std::string& type, const std::string& parameter = {})
{
    return std::any_of(feedback.begin(), feedback.end(), [&](const auto& item) {
        return item.type == type && item.parameter == parameter;
    });
}

void BrowserH264OfferProducesLocalSendOnlyAnswer()
{
    auto session = Session();
    CHECK(session->ApplyRemoteOffer(Header() + Video()));
    std::string text;
    CHECK(session->CreateLocalAnswer(text));
    auto answer = Parse(text, SdpType::Answer);
    CHECK(answer.type == SdpType::Answer);
    CHECK(answer.medias.size() == 1);
    CHECK(answer.bundle.mids == std::vector<std::string>{"video"});
    const auto& video = answer.medias.front();
    CHECK(video.mid == "video" && video.port != 0);
    CHECK(video.direction == MediaDirection::SendOnly);
    CHECK(video.rtcpMux && video.rtcpRsize);
    CHECK(video.codecs.size() == 1);
    CHECK(video.codecs.front().payloadType == 102);
    CHECK(video.codecs.front().clockRate == 90000);
    CHECK(FmtpParameter(video.codecs.front().fmtp, "profile-level-id") == "42e01f");
    CHECK(video.ice.ufrag == "local" && video.ice.pwd == kLocalPassword);
    CHECK(answer.ice.iceLite && video.ice.iceLite);
    CHECK(video.ice.candidates == Options().ice.candidates);
    CHECK(video.ice.endOfCandidates);
    CHECK(video.dtls.setup == DtlsSetup::Active);
    CHECK(video.dtls.fingerprints.size() == 1);
    CHECK(video.dtls.fingerprints.front().value == kLocalFingerprint);
    CHECK(video.ssrcs.size() == 1 && video.ssrcs.front().ssrc == 222222);
    CHECK(video.msids == std::vector<std::string>{"localStream localVideo"});
    CHECK(text.find(kRemotePassword) == std::string::npos);
    CHECK(text.find(kRemoteFingerprint) == std::string::npos);
    CHECK(text.find("remoteStream") == std::string::npos);
    CHECK(text.find("remoteCname") == std::string::npos);
    CHECK(text.find("198.51.100.10") == std::string::npos);
    CHECK(text.find("111111") == std::string::npos);
    CHECK(text.find("a=ssrc-group:FID") == std::string::npos);

    std::string repeated;
    CHECK(session->CreateLocalAnswer(repeated));
    CHECK(repeated == text);
    CHECK(!session->ApplyRemoteOffer(Header() + Video()));
    CHECK(session->State() == WebRtcSessionState::HaveAnswer);
}

void HeaderExtensionsAndFeedbackAreIntersected()
{
    const auto answer = Answer(Header() + Video());
    const auto& video = answer.medias.front();
    CHECK(video.headerExtensions.size() == 2);
    CHECK(video.headerExtensions.front().id == 3);
    CHECK(video.headerExtensions.front().uri == RtpHeaderExtensionUri::SDES_MID);
    CHECK(video.headerExtensions.front().direction == MediaDirection::SendOnly);
    CHECK(video.headerExtensions[1].id == 5);
    CHECK(video.headerExtensions[1].uri == RtpHeaderExtensionUri::TRANSPORT_CC);
    CHECK(video.headerExtensions[1].direction == MediaDirection::SendOnly);
    CHECK(video.codecs.front().rtcpFeedback.size() == 3);
    CHECK(HasFeedback(video.codecs.front().rtcpFeedback, "nack"));
    CHECK(HasFeedback(video.codecs.front().rtcpFeedback, "nack", "pli"));
    CHECK(!HasFeedback(video.codecs.front().rtcpFeedback, "ccm", "fir"));
    CHECK(video.rtcpFeedback.empty());
    CHECK(HasFeedback(video.codecs.front().rtcpFeedback, "transport-cc"));

    auto without_transport_cc = Options();
    without_transport_cc.medias.front().headerExtensions.pop_back();
    const auto fallback = Answer(Header() + Video(), without_transport_cc);
    CHECK(fallback.medias.front().headerExtensions.size() == 1);
    CHECK(!HasFeedback(fallback.medias.front().codecs.front().rtcpFeedback, "transport-cc"));
}

void CodecPreferenceFollowsTheMediaFormats()
{
    auto options = Options();
    options.medias.front().codecs.push_back({120, "VP8", 90000, 0, "", {}});
    // rtpmap still lists H264 first, while m= prefers VP8. Local capability
    // order also prefers H264 and must not override the offer's order.
    const auto offer = Replace(Header() + Video(),
        "m=video 9 UDP/TLS/RTP/SAVPF 102 103 96",
        "m=video 9 UDP/TLS/RTP/SAVPF 96 102 103");
    const auto answer = Answer(offer, options);
    const auto& video = answer.medias.front();
    CHECK((video.fmts == std::vector<std::string>{"96", "102"}));
    CHECK(video.codecs.size() == 2);
    CHECK(video.codecs[0].payloadType == 96 && video.codecs[0].encodingName == "VP8");
    CHECK(video.codecs[1].payloadType == 102 && video.codecs[1].encodingName == "H264");
}

void AmbiguousCapabilitiesAndBundleMappingsAreRejected()
{
    auto missing_codec = Parse(Header() + Video());
    missing_codec.medias.front().codecs.erase(missing_codec.medias.front().codecs.begin());
    auto session = Session();
    CHECK(!session->ApplyRemoteOffer(missing_codec));
    CHECK(session->State() == WebRtcSessionState::New);
    CHECK(!session->LastError().empty());

    auto duplicate_capability = Options();
    duplicate_capability.medias.push_back(duplicate_capability.medias.front());
    session = Session(duplicate_capability);
    CHECK(session->ApplyRemoteOffer(Header() + Video()));
    std::string output = "unchanged";
    CHECK(!session->CreateLocalAnswer(output));
    CHECK(output == "unchanged" && session->State() == WebRtcSessionState::HaveOffer);

    auto conflicting_extensions = Options();
    conflicting_extensions.medias[1].headerExtensions.push_back(
        {1, RtpHeaderExtensionUri::AUDIO_LEVEL, MediaDirection::SendRecv, ""});
    const auto offer = Header("audio video") + Audio() +
        "a=extmap:3 urn:ietf:params:rtp-hdrext:ssrc-audio-level\r\n" + Video();
    session = Session(conflicting_extensions);
    CHECK(session->ApplyRemoteOffer(offer));
    CHECK(!session->CreateLocalAnswer(output));
    CHECK(output == "unchanged" && session->State() == WebRtcSessionState::HaveOffer);
    CHECK(!session->LastError().empty());
}

void ExtensionLimitsAndFeedbackDuplicatesRespectTheAnswer()
{
    auto options = Options();
    options.medias.front().headerExtensions.push_back(
        {7, RtpHeaderExtensionUri::ABS_SEND_TIME, MediaDirection::SendRecv, ""});
    auto offer = Replace(Header() + Video(), "a=extmap:3/recvonly", "a=extmap:3/sendrecv");
    offer += "a=extmap:20 http://www.webrtc.org/experiments/rtp-hdrext/abs-send-time\r\n"
             "a=rtcp-fb:102 transport-cc\r\n";
    const auto answer = Answer(offer, options);
    const auto& video = answer.medias.front();
    CHECK(video.direction == MediaDirection::SendOnly);
    CHECK(video.headerExtensions.size() == 2);
    for (const auto& extension : video.headerExtensions)
    {
        CHECK(extension.id >= 1 && extension.id <= 14);
        CHECK(extension.direction == MediaDirection::SendOnly);
        CHECK(extension.uri != RtpHeaderExtensionUri::ABS_SEND_TIME);
    }
    const auto& feedback = video.codecs.front().rtcpFeedback;
    CHECK(std::count_if(feedback.begin(), feedback.end(), [](const auto& item) {
        return item.type == "transport-cc" && item.parameter.empty();
    }) == 1);
    CHECK(feedback.size() == 3);
}

void DuplicateLocalSourcesAreRejectedWithoutRestrictingReceiveOnlyMedia()
{
    const auto video_sections = Video("left") +
        "m=video 9 UDP/TLS/RTP/SAVPF 104\r\n"
        "c=IN IP4 0.0.0.0\r\n"
        "a=mid:right\r\n"
        "a=rtcp-mux\r\n"
        "a=rtpmap:104 H264/90000\r\n"
        "a=fmtp:104 profile-level-id=42e01f;packetization-mode=1\r\n";
    auto session = Session();
    CHECK(session->ApplyRemoteOffer(Header("left right", "recvonly") + video_sections));
    std::string output = "unchanged";
    CHECK(!session->CreateLocalAnswer(output));
    CHECK(output == "unchanged" && session->State() == WebRtcSessionState::HaveOffer);
    CHECK(!session->LastError().empty());

    auto duplicate_within_media = Options();
    auto& sources = duplicate_within_media.medias.front().ssrcs;
    sources.push_back(sources.front());
    session = Session(duplicate_within_media);
    CHECK(session->ApplyRemoteOffer(Header() + Video()));
    CHECK(!session->CreateLocalAnswer(output));
    CHECK(output == "unchanged" && session->State() == WebRtcSessionState::HaveOffer);

    // Source metadata is not sent for a receive-only answer, so the same
    // capability may still negotiate multiple incoming tracks.
    const auto receiving = Answer(Header("left right", "sendonly") + video_sections);
    CHECK(receiving.medias.size() == 2);
    for (const auto& media : receiving.medias)
    {
        CHECK(media.port != 0 && media.direction == MediaDirection::RecvOnly);
        CHECK(media.ssrcs.empty() && media.msids.empty());
    }

    auto no_declared_sources = Options();
    no_declared_sources.medias.front().ssrcs.clear();
    const auto sending = Answer(Header("left right", "recvonly") + video_sections,
                                no_declared_sources);
    CHECK(sending.medias.size() == 2);
    for (const auto& media : sending.medias)
    {
        CHECK(media.port != 0 && media.direction == MediaDirection::SendOnly);
        CHECK(media.ssrcs.empty());
    }
}

void FmtpParameterOrderAndCaseDoNotPreventH264Negotiation()
{
    const auto answer = Answer(Header() + Video("video",
        "PROFILE-LEVEL-ID=42E01F; level-asymmetry-allowed=1 ; PACKETIZATION-MODE=1"));
    const auto& codec = answer.medias.front().codecs.front();
    CHECK(codec.payloadType == 102);
    CHECK(FmtpParameter(codec.fmtp, "profile-level-id") == "42e01f");
    CHECK(FmtpParameter(codec.fmtp, "packetization-mode") == "1");
}

void IncompatibleH264ProfilesAndPacketizationAreRejected()
{
    for (const auto& fmtp : {
             "profile-level-id=640c1f;packetization-mode=1",
             "profile-level-id=42e01f;packetization-mode=0"})
    {
        auto session = Session();
        CHECK(session->ApplyRemoteOffer(Header() + Video("video", fmtp)));
        std::string answer = "unchanged";
        CHECK(!session->CreateLocalAnswer(answer));
        CHECK(answer == "unchanged");
        CHECK(session->State() == WebRtcSessionState::HaveOffer);
        CHECK(!session->LastError().empty());
    }
}

void H264LevelRespectsBothPeersWithoutAsymmetry()
{
    auto options = Options();
    options.medias.front().codecs.front().fmtp =
        "profile-level-id=42e02a;packetization-mode=1;level-asymmetry-allowed=0";
    auto answer = Answer(Header() + Video("video",
        "packetization-mode=1;profile-level-id=42e01f;level-asymmetry-allowed=1"), options);
    CHECK(FmtpParameter(answer.medias.front().codecs.front().fmtp, "profile-level-id") == "42e01f");

    options.medias.front().codecs.front().fmtp =
        "profile-level-id=42e01f;packetization-mode=1;level-asymmetry-allowed=0";
    answer = Answer(Header() + Video("video", "packetization-mode=1;profile-level-id=42e02a"), options);
    CHECK(FmtpParameter(answer.medias.front().codecs.front().fmtp, "profile-level-id") == "42e01f");
}

void OpusAnswerAdvertisesLocalReceiveParameters()
{
    const auto answer = Answer(Header("audio", "sendonly") + Audio());
    const auto& audio = answer.medias.front();
    CHECK(audio.direction == MediaDirection::RecvOnly);
    CHECK(audio.codecs.size() == 1);
    const auto& codec = audio.codecs.front();
    CHECK(codec.payloadType == 111 && codec.clockRate == 48000 && codec.channels == 2);
    CHECK(FmtpParameter(codec.fmtp, "minptime") == "10");
    CHECK(FmtpParameter(codec.fmtp, "useinbandfec") == "1");
    CHECK(FmtpParameter(codec.fmtp, "stereo") == "0");
    CHECK(audio.ssrcs.empty() && audio.msids.empty());
}

void RejectedMediaKeepTheirPositionAndLeaveTheBundle()
{
    const auto offer = Header("unused audio data video") +
        "m=video 9 UDP/TLS/RTP/SAVPF 98\r\n"
        "c=IN IP4 0.0.0.0\r\n"
        "a=mid:unused\r\n"
        "a=rtcp-mux\r\n"
        "a=rtpmap:98 AV1/90000\r\n" + Audio() +
        "m=application 9 UDP/DTLS/SCTP webrtc-datachannel\r\n"
        "c=IN IP4 0.0.0.0\r\n"
        "a=mid:data\r\n"
        "a=sctp-port:5000\r\n" + Video();
    const auto answer = Answer(offer);
    CHECK(answer.medias.size() == 4);
    CHECK(answer.medias[0].mid == "unused" && answer.medias[0].port == 0);
    CHECK(answer.medias[1].mid == "audio" && answer.medias[1].port != 0);
    CHECK(answer.medias[2].mid == "data" && answer.medias[2].port == 0);
    CHECK(answer.medias[3].mid == "video" && answer.medias[3].port != 0);
    CHECK(answer.medias[0].direction == MediaDirection::Inactive);
    CHECK(answer.medias[2].direction == MediaDirection::Inactive);
    CHECK(answer.medias[2].proto == "UDP/DTLS/SCTP");
    CHECK(answer.medias[2].fmts == std::vector<std::string>{"webrtc-datachannel"});
    CHECK((answer.bundle.mids == std::vector<std::string>{"audio", "video"}));
}

void SessionDefaultsAreInheritedAndMediaOverridesWin()
{
    auto offer = Header("audio video", "sendonly") + Audio() + Video() +
        "a=recvonly\r\n"
        "a=ice-ufrag:videoRemote\r\n"
        "a=ice-pwd:videoRemotePassword01234567\r\n"
        "a=setup:passive\r\n";
    const auto parsed = Parse(offer);
    CHECK(parsed.medias.size() == 2);
    const auto& audio = parsed.medias[0];
    CHECK(audio.direction == MediaDirection::SendOnly);
    CHECK(audio.ice.ufrag == "remote" && audio.ice.pwd == kRemotePassword);
    CHECK(audio.dtls.setup == DtlsSetup::ActPass);
    CHECK(audio.dtls.fingerprints.front().value == kRemoteFingerprint);
    const auto& video = parsed.medias[1];
    CHECK(video.direction == MediaDirection::RecvOnly);
    CHECK(video.ice.ufrag == "videoRemote");
    CHECK(video.ice.pwd == "videoRemotePassword01234567");
    CHECK(video.dtls.setup == DtlsSetup::Passive);
    CHECK(video.dtls.fingerprints.front().value == kRemoteFingerprint);
    CHECK(video.ssrcs.size() == 2);
    CHECK(video.ssrcGroups.size() == 1);
    CHECK((video.ssrcGroups.front().ssrcs == std::vector<uint32_t>{111111, 111112}));
}

void DtlsAnswerUsesTheComplementaryRole()
{
    auto offer = Replace(Header() + Video(), "a=setup:actpass", "a=setup:active");
    const auto answer = Answer(offer);
    CHECK(answer.medias.front().dtls.setup == DtlsSetup::Passive);
}

std::string Fingerprint(size_t bytes, const std::string& octet)
{
    std::string result;
    for (size_t i = 0; i < bytes; ++i)
    {
        if (i) result += ':';
        result += octet;
    }
    return result;
}

void BundledFingerprintSetsIgnoreOrderAndHexCase()
{
    const auto offer = Header("audio video") + Audio() +
        "a=fingerprint:sha-256 " + Fingerprint(32, "aa") + "\r\n" +
        "a=fingerprint:sha-384 " + Fingerprint(48, "bb") + "\r\n" + Video() +
        "a=fingerprint:sha-384 " + Fingerprint(48, "BB") + "\r\n" +
        "a=fingerprint:sha-256 " + Fingerprint(32, "AA") + "\r\n";
    auto session = Session();
    CHECK(session->ApplyRemoteOffer(offer));

    const auto different_certificate = Replace(offer,
        "a=fingerprint:sha-256 " + Fingerprint(32, "AA"),
        "a=fingerprint:sha-256 " + Fingerprint(32, "CC"));
    CHECK(!session->ApplyRemoteOffer(different_certificate));
    CHECK(session->State() == WebRtcSessionState::HaveOffer);
    CHECK(!session->LastError().empty());

    std::string answer_text;
    CHECK(session->CreateLocalAnswer(answer_text));
    const auto answer = Parse(answer_text, SdpType::Answer);
    CHECK(answer.medias.size() == 2);
    CHECK(answer.medias[0].port != 0 && answer.medias[1].port != 0);
    CHECK((answer.bundle.mids == std::vector<std::string>{"audio", "video"}));
    for (const auto& media : answer.medias)
    {
        CHECK(media.dtls.fingerprints.size() == 1);
        CHECK(media.dtls.fingerprints.front().value == kLocalFingerprint);
    }
}

void MalformedInputDoesNotReplaceTheParsedDescription()
{
    const auto valid = Header() + Video();
    const std::vector<std::string> invalid = {
        "",
        Replace(valid, "m=video 9 ", "m=video invalid "),
        Replace(valid, "H264/90000", "H264/not-a-rate"),
        Replace(valid, "a=mid:video\r\n", "a=mid:video\r\na=mid:other\r\n"),
        Replace(valid, "a=rtpmap:102 H264/90000\r\n",
            "a=rtpmap:102 H264/90000\r\na=rtpmap:102 VP8/90000\r\n"),
        Replace(valid, "a=setup:actpass\r\n", "a=setup:actpass\r\na=setup:active\r\n"),
        Header("video") + Video() + Video(),
        Replace(valid, "a=group:BUNDLE video", "a=group:BUNDLE missing"),
    };
    for (size_t index = 0; index < invalid.size(); ++index)
    {
        WebRtcSessionDescription output;
        output.type = SdpType::Answer;
        output.ice.ufrag = "sentinel";
        output.medias.resize(1);
        output.medias.front().mid = "unchanged";
        std::string error;
        if (sdp::Sdp::Parse(invalid[index], sdp::SdpProfile::WebRtc, SdpType::Offer, output, error))
            throw std::runtime_error("Malformed SDP was accepted, case " + std::to_string(index));
        CHECK(!error.empty());
        CHECK(output.type == SdpType::Answer);
        CHECK(output.ice.ufrag == "sentinel");
        CHECK(output.medias.size() == 1 && output.medias.front().mid == "unchanged");
    }
}

void FailedOfferAndAnswerKeepThePreviousState()
{
    auto session = Session();
    std::string output = "unchanged";
    CHECK(!session->CreateLocalAnswer(output));
    CHECK(output == "unchanged" && session->State() == WebRtcSessionState::New);
    CHECK(!session->ApplyRemoteOffer("this is not SDP"));
    CHECK(session->State() == WebRtcSessionState::New);
    CHECK(session->ApplyRemoteOffer(Header() + Video()));
    CHECK(!session->ApplyRemoteOffer(Replace(Header() + Video(), "H264/90000", "H264/bad")));
    CHECK(session->State() == WebRtcSessionState::HaveOffer);
    CHECK(!session->LastError().empty());
    CHECK(session->CreateLocalAnswer(output));
    CHECK(Parse(output, SdpType::Answer).medias.front().mid == "video");
    CHECK(session->LastError().empty());

    auto no_candidate = Options();
    no_candidate.ice.candidates.clear();
    auto incomplete = Session(no_candidate);
    CHECK(incomplete->ApplyRemoteOffer(Header() + Video()));
    output = "unchanged";
    CHECK(!incomplete->CreateLocalAnswer(output));
    CHECK(output == "unchanged");
    CHECK(incomplete->State() == WebRtcSessionState::HaveOffer);
}

void SerializedDescriptionsCanBeParsedAgain()
{
    auto original = Parse(Header("audio video") + Audio() + Video());
    const auto text = sdp::Sdp::Serialize(original);
    CHECK(!text.empty());
    auto roundtrip = Parse(text);
    CHECK(roundtrip.medias.size() == original.medias.size());
    CHECK(roundtrip.bundle.mids == original.bundle.mids);
    for (size_t i = 0; i < original.medias.size(); ++i)
    {
        CHECK(roundtrip.medias[i].mid == original.medias[i].mid);
        CHECK(roundtrip.medias[i].media == original.medias[i].media);
        CHECK(roundtrip.medias[i].direction == original.medias[i].direction);
        CHECK(roundtrip.medias[i].codecs.size() == original.medias[i].codecs.size());
        CHECK(roundtrip.medias[i].ice.ufrag == original.medias[i].ice.ufrag);
        CHECK(roundtrip.medias[i].dtls.fingerprints.front().value == kRemoteFingerprint);
    }

    auto session = Session();
    CHECK(session->ApplyRemoteOffer(original));
    WebRtcSessionDescription typed_answer;
    CHECK(session->CreateLocalAnswer(typed_answer));
    const auto answer_text = sdp::Sdp::Serialize(typed_answer);
    CHECK(!answer_text.empty());
    auto answer = Parse(answer_text, SdpType::Answer);
    CHECK(answer.medias.size() == 2 && answer.ice.iceLite);
    CHECK(answer.medias[0].ice.pwd == kLocalPassword);
    CHECK(answer.medias[1].ice.pwd == kLocalPassword);
}
} // namespace

int main()
{
    const std::pair<const char*, std::function<void()>> tests[] = {
        {"browser H264 offer and local answer", BrowserH264OfferProducesLocalSendOnlyAnswer},
        {"extension and feedback intersection", HeaderExtensionsAndFeedbackAreIntersected},
        {"m-line codec preference", CodecPreferenceFollowsTheMediaFormats},
        {"ambiguous capabilities and BUNDLE mappings", AmbiguousCapabilitiesAndBundleMappingsAreRejected},
        {"extension limits and feedback deduplication", ExtensionLimitsAndFeedbackDuplicatesRespectTheAnswer},
        {"local SSRC collisions and receive-only media", DuplicateLocalSourcesAreRejectedWithoutRestrictingReceiveOnlyMedia},
        {"H264 fmtp order and case", FmtpParameterOrderAndCaseDoNotPreventH264Negotiation},
        {"H264 profile and mode rejection", IncompatibleH264ProfilesAndPacketizationAreRejected},
        {"H264 level negotiation", H264LevelRespectsBothPeersWithoutAsymmetry},
        {"Opus local receive parameters", OpusAnswerAdvertisesLocalReceiveParameters},
        {"rejected m-line order and BUNDLE", RejectedMediaKeepTheirPositionAndLeaveTheBundle},
        {"session defaults and media overrides", SessionDefaultsAreInheritedAndMediaOverridesWin},
        {"complementary DTLS role", DtlsAnswerUsesTheComplementaryRole},
        {"BUNDLE fingerprint sets and certificate mismatch", BundledFingerprintSetsIgnoreOrderAndHexCase},
        {"malformed SDP preserves parsed output", MalformedInputDoesNotReplaceTheParsedDescription},
        {"negotiation failure preserves state", FailedOfferAndAnswerKeepThePreviousState},
        {"serialization and typed API round trip", SerializedDescriptionsCanBeParsedAgain},
    };
    int failures = 0;
    for (const auto& [name, test] : tests)
    {
        try
        {
            test();
            std::cout << "PASS " << name << '\n';
        }
        catch (const std::exception& error)
        {
            ++failures;
            std::cerr << "FAIL " << name << ": " << error.what() << '\n';
        }
    }
    return failures == 0 ? 0 : 1;
}
