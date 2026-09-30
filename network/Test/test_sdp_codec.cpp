#include "Sdp.h"
#include "SdpCodec.h"

#include <algorithm>
#include <iostream>
#include <sstream>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace
{
#define CHECK(value) do { if (!(value)) throw std::runtime_error( \
    std::string(__func__) + ":" + std::to_string(__LINE__) + ": " + #value); } while (false)

using sdp::RtpCodecParameters;
using sdp::Sdp;
using sdp::SdpMedia;
using sdp::SdpProfile;
using sdp::SdpSession;
using sdp::SdpCodec;

bool Has(const RtpCodecParameters& codec, const std::string& parameter)
{
    return (";" + codec.fmtp + ";").find(";" + parameter + ";") != std::string::npos;
}

template<class Function>
void Rejects(Function&& function)
{
    bool rejected = false;
    try { function(); }
    catch (const std::invalid_argument&) { rejected = true; }
    CHECK(rejected);
}

size_t Occurrences(const std::string& text, const std::string& part)
{
    size_t result = 0;
    for (size_t offset = 0; (offset = text.find(part, offset)) != std::string::npos; offset += part.size())
        ++result;
    return result;
}

SdpSession Parse(const std::string& text, SdpProfile profile)
{
    SdpSession result;
    std::string error;
    if (!Sdp::Parse(text, profile, sdp::SdpType::Offer, result, error))
        throw std::runtime_error("SDP parse failed: " + error);
    CHECK(error.empty());
    return result;
}

std::string VideoDescription()
{
    return "v=0\r\no=- 7 1 IN IP4 127.0.0.1\r\ns=codec-test\r\nt=0 0\r\n"
        "a=group:BUNDLE video\r\n"
        "m=video 5004 UDP/TLS/RTP/SAVPF 96 97\r\nc=IN IP4 192.0.2.1\r\n"
        "a=mid:video\r\na=sendonly\r\na=rtcp-mux\r\n"
        "a=ice-ufrag:test\r\na=ice-pwd:abcdefghijklmnopqrstuvwxyz\r\n"
        "a=fingerprint:sha-256 00:01:02:03:04:05:06:07:08:09:0A:0B:0C:0D:0E:0F:10:11:12:13:14:15:16:17:18:19:1A:1B:1C:1D:1E:1F\r\n"
        "a=setup:actpass\r\na=candidate:1 1 UDP 2130706431 192.0.2.1 5004 typ host\r\na=end-of-candidates\r\n"
        "a=rtpmap:96 H264/90000\r\na=fmtp:96 packetization-mode=1;profile-level-id=42e01f\r\n"
        "a=rtpmap:97 rtx/90000\r\na=fmtp:97 apt=96\r\n"
        "a=rtcp-fb:96 nack\r\na=rtcp-fb:97 nack pli\r\na=rtcp-fb:* ccm fir\r\n"
        "a=control:trackID=0\r\na=x-application:keep-this\r\n";
}

// Generic serialization consumes legacy maps, while WebRTC consumes codecs.
// Compare both views to detect partial mutation on a rejected replacement.
std::string MediaSnapshot(const SdpMedia& media)
{
    SdpSession session;
    session.medias = {media};
    std::ostringstream result;
    result << Sdp::Serialize(session);
    for (const auto& codec : media.codecs)
    {
        result << codec.payloadType << ':' << codec.encodingName << ':' << codec.clockRate << ':'
               << codec.channels << ':' << codec.fmtp << '\n';
        for (const auto& feedback : codec.rtcpFeedback)
            result << feedback.type << ':' << feedback.parameter << '\n';
    }
    for (const auto& map : media.rtpmaps)
        result << map.payloadType << ':' << map.encodingName << ':' << map.clockRate << ':' << map.channels << '\n';
    for (const auto& format : media.fmtps) result << format.payloadType << ':' << format.params << '\n';
    for (const auto& feedback : media.rtcpFeedback) result << feedback.type << ':' << feedback.parameter << '\n';
    result << media.mid << ':' << static_cast<int>(media.direction) << ':' << media.rtcpMux << ':'
           << media.ice.ufrag << ':' << media.ice.pwd;
    return result.str();
}

void RejectsWithoutChangingMedia(SdpMedia& media, std::vector<RtpCodecParameters> codecs)
{
    const auto before = MediaSnapshot(media);
    Rejects([&] { Sdp::SetCodecs(media, std::move(codecs)); });
    CHECK(MediaSnapshot(media) == before);
}

void H264RuntimeConfigurationAndSpsProfile()
{
    sdp::H264CodecConfig config;
    auto codec = SdpCodec::H264(96, config);
    CHECK(codec.encodingName == "H264" && codec.clockRate == 90000 && codec.channels <= 1);
    CHECK(codec.payloadType == 96 && Has(codec, "profile-level-id=42e01f"));
    CHECK(Has(codec, "packetization-mode=1") && Has(codec, "level-asymmetry-allowed=1"));
    config.profileLevelId = 0x640028;
    config.packetizationMode = 0;
    config.levelAsymmetryAllowed = false;
    codec = SdpCodec::H264(110, config);
    CHECK(codec.payloadType == 110 && Has(codec, "profile-level-id=640028"));
    CHECK(Has(codec, "packetization-mode=0") && Has(codec, "level-asymmetry-allowed=0"));
    // The actual SPS profile must override a conflicting configuration value.
    config.sps = {0x67, 0x42, 0xe0, 0x1f, 0x95, 0xa8, 0x14, 0x01, 0x6e, 0x40};
    config.pps = {0x68, 0xce, 0x3c, 0x80};
    codec = SdpCodec::H264(111, config);
    CHECK(Has(codec, "profile-level-id=42e01f"));
    CHECK(Has(codec, "sprop-parameter-sets=Z0LgH5WoFAFuQA==,aM48gA=="));
    config.pps.clear();
    Rejects([&] { SdpCodec::H264(96, config); });
    config.sps.clear();
    config.packetizationMode = 3;
    Rejects([&] { SdpCodec::H264(96, config); });
    config.packetizationMode = 1;
    for (const uint32_t profile : {0x1000000U, 0x42e11fU, 0x42e0ffU, 0x42e009U})
    {
        config.profileLevelId = profile;
        Rejects([&] { SdpCodec::H264(96, config); });
    }
    config.profileLevelId = 0x640009;
    CHECK(Has(SdpCodec::H264(96, config), "profile-level-id=640009"));
    config.sps = {0x67, 0x42, 0xe1, 0x1f}; // Invalid reserved bits in the actual SPS header.
    config.pps = {0x68, 0xce, 0x3c, 0x80};
    Rejects([&] { SdpCodec::H264(96, config); });
}

void H265ParameterSetsAndRuntimeConfiguration()
{
    sdp::H265CodecConfig config;
    auto codec = SdpCodec::H265(98, config);
    CHECK(codec.encodingName == "H265" && codec.clockRate == 90000 && codec.payloadType == 98);
    CHECK(Has(codec, "profile-id=1") && Has(codec, "level-id=93"));
    CHECK(codec.fmtp.find("sprop-") == std::string::npos);
    config.profileSpace = 1;
    config.profileId = 2;
    config.tierFlag = true;
    config.levelId = 120;
    config.vps = {0x40, 0x01, 0x0c, 0x01, 0xff, 0xff};
    config.sps = {0x42, 0x01, 0x01, 0x01, 0x60, 0x00};
    config.pps = {0x44, 0x01, 0xc0, 0x73};
    codec = SdpCodec::H265(114, config);
    CHECK(codec.payloadType == 114 && Has(codec, "profile-space=1") && Has(codec, "profile-id=2"));
    CHECK(Has(codec, "tier-flag=1") && Has(codec, "level-id=120"));
    CHECK(Has(codec, "sprop-vps=QAEMAf//") && Has(codec, "sprop-sps=QgEBAWAA") &&
        Has(codec, "sprop-pps=RAHAcw=="));
    config.profileSpace = 4;
    Rejects([&] { SdpCodec::H265(98, config); });
    config.profileSpace = 0;
    config.profileId = 32;
    Rejects([&] { SdpCodec::H265(98, config); });
    config.profileId = 1;
    config.vps = {0x40, 0x00}; // nuh_temporal_id_plus1 cannot be zero.
    Rejects([&] { SdpCodec::H265(98, config); });
    config.vps.clear();
    config.levelId = 93; // High tier starts at HEVC Level 4 (level_idc=120).
    Rejects([&] { SdpCodec::H265(98, config); });
}

void OpusFixedRtpFormatAndLocalPreferences()
{
    sdp::OpusCodecConfig config;
    auto codec = SdpCodec::Opus(111, config);
    CHECK(codec.encodingName == "opus" && codec.clockRate == 48000 && codec.channels == 2);
    CHECK(codec.fmtp.empty());
    config.maxAverageBitrate = 32000;
    config.maxPlaybackRate = 16000;
    config.minPtime = 10;
    config.stereo = false;
    config.useInbandFec = true;
    codec = SdpCodec::Opus(112, config);
    CHECK(codec.payloadType == 112 && codec.clockRate == 48000 && codec.channels == 2);
    CHECK(Has(codec, "maxaveragebitrate=32000") && Has(codec, "maxplaybackrate=16000"));
    CHECK(Has(codec, "minptime=10") && Has(codec, "stereo=0") && Has(codec, "useinbandfec=1"));
    config.maxAverageBitrate = 64000;
    config.stereo = true;
    config.useInbandFec = false;
    codec = SdpCodec::Opus(113, config);
    CHECK(Has(codec, "maxaveragebitrate=64000") && Has(codec, "stereo=1") && Has(codec, "useinbandfec=0"));
    CHECK(!Has(codec, "maxaveragebitrate=32000"));
    config.maxAverageBitrate = 5999;
    Rejects([&] { SdpCodec::Opus(111, config); });
    config.maxAverageBitrate = 64000;
    config.minPtime = 121;
    Rejects([&] { SdpCodec::Opus(111, config); });
}

void AacAudioSpecificConfigKnownVectors()
{
    sdp::AacCodecConfig config;
    config.sampleRate = 44100;
    auto codec = SdpCodec::Aac(97, config);
    CHECK(codec.encodingName == "MPEG4-GENERIC" && codec.clockRate == 44100 && codec.channels == 2);
    CHECK(Has(codec, "config=1210") && Has(codec, "streamtype=5") && Has(codec, "mode=AAC-hbr"));
    CHECK(Has(codec, "sizeLength=13") && Has(codec, "indexLength=3") && Has(codec, "indexDeltaLength=3"));
    config.sampleRate = 48000;
    codec = SdpCodec::Aac(99, config);
    CHECK(codec.payloadType == 99 && codec.clockRate == 48000 && Has(codec, "config=1190"));
    config.channels = 1;
    codec = SdpCodec::Aac(100, config);
    CHECK(codec.channels == 1 && Has(codec, "config=1188"));
    CHECK(Has(codec, "sizeLength=13") && Has(codec, "indexLength=3") && Has(codec, "indexDeltaLength=3"));
    config.channels = 8;
    codec = SdpCodec::Aac(101, config);
    CHECK(codec.channels == 8 && Has(codec, "config=11b8"));
    config.channels = 7; // ASC configuration 7 describes eight channels, not seven.
    Rejects([&] { SdpCodec::Aac(97, config); });
    config.channels = 2;
    config.sampleRate = 12345;
    Rejects([&] { SdpCodec::Aac(97, config); });
    config.sampleRate = 48000;
    config.channels = 0;
    Rejects([&] { SdpCodec::Aac(97, config); });
}

void AacProfileLevelsFollowRateAndLayout()
{
    // MPEG-4 Audio Profile indication differs from the AAC object type. Level
    // 4/5 includes a 5.1 layout; Level 6/7 includes 7.1 and uses IDs 80/81.
    const int cases[][3] = {
        {24000, 2, 40}, {32000, 2, 41}, {48000, 2, 41},
        {48000, 6, 42}, {64000, 2, 43}, {96000, 6, 43},
        {48000, 8, 80}, {96000, 8, 81}
    };
    for (const auto& item : cases)
    {
        sdp::AacCodecConfig config;
        config.sampleRate = item[0];
        config.channels = item[1];
        const auto codec = SdpCodec::Aac(97, config);
        CHECK(Has(codec, "profile-level-id=" + std::to_string(item[2])));
    }
}

void AmrWidebandModeSetsAndDependencies()
{
    sdp::AmrWbCodecConfig config;
    auto codec = SdpCodec::AmrWb(100, config);
    CHECK(codec.encodingName == "AMR-WB" && codec.clockRate == 16000 && codec.channels == 1);
    CHECK(Has(codec, "octet-align=1") && codec.fmtp.find("mode-set") == std::string::npos);
    config.channels = 2;
    config.modeSet = {0, 2, 8};
    config.crc = true;
    config.robustSorting = true;
    config.interleaving = 2;
    codec = SdpCodec::AmrWb(101, config);
    CHECK(codec.payloadType == 101 && codec.channels == 2 && codec.clockRate == 16000);
    CHECK(Has(codec, "mode-set=0,2,8") && Has(codec, "crc=1") && Has(codec, "robust-sorting=1"));
    CHECK(Has(codec, "interleaving=2"));
    config.modeSet = {1, 4};
    codec = SdpCodec::AmrWb(102, config);
    CHECK(Has(codec, "mode-set=1,4") && !Has(codec, "mode-set=0,2,8"));
    config.modeSet = {1, 1};
    Rejects([&] { SdpCodec::AmrWb(100, config); });
    config.modeSet = {9};
    Rejects([&] { SdpCodec::AmrWb(100, config); });
    config.modeSet = {0};
    config.octetAlign = false;
    Rejects([&] { SdpCodec::AmrWb(100, config); });
    config.octetAlign = true;
    config.interleaving = 0;
    Rejects([&] { SdpCodec::AmrWb(100, config); });
    config.interleaving = 2;
    config.channels = 7;
    Rejects([&] { SdpCodec::AmrWb(100, config); });
}

void BuildersRejectInvalidPayloadTypes()
{
    for (const int pt : {-1, 128})
    {
        Rejects([&] { SdpCodec::H264(pt); });
        Rejects([&] { SdpCodec::H265(pt); });
        Rejects([&] { SdpCodec::Opus(pt); });
        Rejects([&] { SdpCodec::Aac(pt); });
        Rejects([&] { SdpCodec::AmrWb(pt); });
    }
}

void AllBuilderDescriptionsRoundTrip()
{
    const std::vector<RtpCodecParameters> codecs = {
        SdpCodec::H264(96), SdpCodec::H265(98), SdpCodec::Opus(111),
        SdpCodec::Aac(99), SdpCodec::AmrWb(100)
    };
    for (const auto profile : {SdpProfile::Generic, SdpProfile::WebRtc})
        for (const auto& codec : codecs)
        {
            auto session = Parse(VideoDescription(), profile);
            auto& media = session.medias.front();
            media.media = codec.encodingName == "H264" || codec.encodingName == "H265" ? "video" : "audio";
            Sdp::SetCodecs(media, {codec});
            const auto text = Sdp::Serialize(session);
            const auto parsed = Parse(text, profile);
            const auto& roundTrip = parsed.medias.front();
            CHECK(roundTrip.fmts == std::vector<std::string>{std::to_string(codec.payloadType)});
            if (profile == SdpProfile::WebRtc)
            {
                CHECK(roundTrip.codecs.size() == 1);
                const auto& result = roundTrip.codecs.front();
                CHECK(result.encodingName == codec.encodingName && result.clockRate == codec.clockRate);
                CHECK(std::max(1, result.channels) == std::max(1, codec.channels));
                CHECK(result.fmtp == codec.fmtp);
            }
            else
            {
                CHECK(roundTrip.rtpmaps.size() == 1);
                const auto& result = roundTrip.rtpmaps.front();
                CHECK(result.encodingName == codec.encodingName && result.clockRate == codec.clockRate);
                CHECK(std::max(1, result.channels) == std::max(1, codec.channels));
                if (codec.fmtp.empty()) CHECK(roundTrip.fmtps.empty());
                else CHECK(roundTrip.fmtps.size() == 1 && roundTrip.fmtps.front().params == codec.fmtp);
            }
        }
}

void CodecReplacementRoundTrips()
{
    for (const auto profile : {SdpProfile::Generic, SdpProfile::WebRtc})
    {
        auto session = Parse(VideoDescription(), profile);
        auto& media = session.medias.front();
        // Parsed WebRTC clears modeled raw attributes; exercise the cleanup
        // path even when an application retains stale generic attributes.
        media.attributes.push_back({"rtpmap", "98 H264/90000"});
        media.attributes.push_back({"fmtp", "98 profile-level-id=42001f"});
        media.attributes.push_back({"rtcp-fb", "98 nack"});
        sdp::H264CodecConfig config;
        config.profileLevelId = 0x42e01e;
        auto replacement = SdpCodec::H264(112, config);
        replacement.rtcpFeedback = {{"nack", "pli"}};
        Sdp::SetCodecs(media, {replacement});
        CHECK(media.fmts == std::vector<std::string>{"112"});
        CHECK(media.codecs.size() == 1 && media.codecs.front().payloadType == 112);
        CHECK(media.rtpmaps.size() == 1 && media.rtpmaps.front().payloadType == 112);
        CHECK(media.fmtps.size() == 1 && media.fmtps.front().payloadType == 112);
        CHECK(media.GetAttribute("control") == "trackID=0");
        CHECK(media.GetAttribute("x-application") == "keep-this");
        CHECK(media.proto == "UDP/TLS/RTP/SAVPF" && media.port == 5004);
        const auto text = Sdp::Serialize(session);
        CHECK(text.find("m=video 5004 UDP/TLS/RTP/SAVPF 112\r\n") != std::string::npos);
        CHECK(Occurrences(text, "a=rtpmap:112 ") == 1);
        CHECK(Occurrences(text, "a=fmtp:112 ") == 1);
        CHECK(Occurrences(text, "a=rtcp-fb:* ccm fir\r\n") == 1);
        CHECK(Occurrences(text, "a=rtcp-fb:112 nack pli\r\n") == 1);
        for (const auto old : {96, 97, 98})
        {
            CHECK(text.find("a=rtpmap:" + std::to_string(old) + " ") == std::string::npos);
            CHECK(text.find("a=fmtp:" + std::to_string(old) + " ") == std::string::npos);
            CHECK(text.find("a=rtcp-fb:" + std::to_string(old) + " ") == std::string::npos);
        }
        CHECK(text.find("a=ice-ufrag:test\r\n") != std::string::npos);
        auto roundTrip = Parse(text, profile);
        CHECK(roundTrip.medias.front().fmts == std::vector<std::string>{"112"});
        if (profile == SdpProfile::WebRtc)
        {
            CHECK(roundTrip.medias.front().codecs.front().fmtp == replacement.fmtp);
            CHECK(roundTrip.medias.front().ice.ufrag == "test");
            CHECK(roundTrip.medias.front().mid == "video");
        }
        else CHECK(roundTrip.medias.front().fmtps.front().params == replacement.fmtp);

        config.profileLevelId = 0x64001f;
        replacement = SdpCodec::H264(113, config);
        Sdp::SetCodecs(roundTrip.medias.front(), {replacement});
        const auto updated = Sdp::Serialize(roundTrip);
        CHECK(updated.find("a=rtpmap:112 ") == std::string::npos);
        CHECK(updated.find("a=fmtp:112 ") == std::string::npos);
        CHECK(updated.find("a=rtcp-fb:112 ") == std::string::npos);
        CHECK(updated.find("profile-level-id=42e01e") == std::string::npos);
        CHECK(Occurrences(updated, "a=rtpmap:113 ") == 1);
        CHECK(Occurrences(updated, "a=fmtp:113 ") == 1);
        const auto final = Parse(updated, profile);
        CHECK(final.medias.front().fmts == std::vector<std::string>{"113"});
        CHECK(Sdp::Serialize(final) == updated);
    }
}

void CodecReplacementRejectsInvalidInputTransactionally()
{
    auto session = Parse(VideoDescription(), SdpProfile::WebRtc);
    auto media = session.medias.front();
    const RtpCodecParameters valid{112, "H264", 90000, 0,
        "packetization-mode=1;profile-level-id=42e01f", {}};
    auto invalid = valid;
    for (const int pt : {-1, 128})
    {
        invalid = valid;
        invalid.payloadType = pt;
        RejectsWithoutChangingMedia(media, {invalid});
    }
    RejectsWithoutChangingMedia(media, {valid, valid});
    RejectsWithoutChangingMedia(media, {});
    invalid = valid;
    invalid.clockRate = 0;
    RejectsWithoutChangingMedia(media, {invalid});
    invalid = valid;
    invalid.channels = -1;
    RejectsWithoutChangingMedia(media, {invalid});
    invalid = valid;
    invalid.encodingName = "H264\r\na=sendonly";
    RejectsWithoutChangingMedia(media, {invalid});
    invalid = valid;
    invalid.fmtp = "packetization-mode=1\r\na=sendrecv";
    RejectsWithoutChangingMedia(media, {invalid});
    invalid.fmtp = " \t ";
    RejectsWithoutChangingMedia(media, {invalid});
    invalid = valid;
    invalid.rtcpFeedback = {{"nack", "pli\na=sendrecv"}};
    RejectsWithoutChangingMedia(media, {invalid});
    // Reject the whole update even when the first replacement was valid.
    RejectsWithoutChangingMedia(media, {valid, invalid});
    invalid = valid;
    invalid.rtcpFeedback = {{"nack", "pli"}, {"nack", " pli "}};
    RejectsWithoutChangingMedia(media, {invalid});
    auto normalized = valid;
    normalized.rtcpFeedback = {{"nack", "  pli\t"}, {"nack", ""}};
    Sdp::SetCodecs(media, {normalized});
    CHECK(media.codecs.front().rtcpFeedback.size() == 2);
    CHECK(media.codecs.front().rtcpFeedback.front().parameter == "pli");
    CHECK(media.codecs.front().rtcpFeedback.back().parameter.empty());
    media.media = "application";
    RejectsWithoutChangingMedia(media, {valid});
    media.media = "audio";
    RejectsWithoutChangingMedia(media, {valid});
    media.media = "video";
    RejectsWithoutChangingMedia(media, {SdpCodec::Opus(111)});
    media.proto = "UDP/DTLS/SCTP";
    RejectsWithoutChangingMedia(media, {valid});
}
}

int main()
{
    try
    {
        H264RuntimeConfigurationAndSpsProfile();
        H265ParameterSetsAndRuntimeConfiguration();
        OpusFixedRtpFormatAndLocalPreferences();
        AacAudioSpecificConfigKnownVectors();
        AacProfileLevelsFollowRateAndLayout();
        AmrWidebandModeSetsAndDependencies();
        BuildersRejectInvalidPayloadTypes();
        AllBuilderDescriptionsRoundTrip();
        CodecReplacementRoundTrips();
        CodecReplacementRejectsInvalidInputTransactionally();
        std::cout << "SDP codec builders and replacement tests passed\n";
        return 0;
    }
    catch (const std::exception& error)
    {
        std::cerr << "SDP codec test failed: " << error.what() << '\n';
        return 1;
    }
}
