#include "WebRtcCodec.h"

#include <iostream>
#include <stdexcept>
#include <string>

using namespace protocol::webrtc;

#define CHECK(value) do { if (!(value)) throw std::runtime_error( \
    std::string("line ") + std::to_string(__LINE__) + ": " + #value); } while (false)

namespace
{
RtpCodecParameters H264(const std::string& fmtp)
{
    return {96, "H264", 90000, 0, fmtp, {{"nack", ""}}};
}

bool Has(const RtpCodecParameters& codec, const std::string& parameter)
{
    return (";" + codec.fmtp + ";").find(";" + parameter + ";") != std::string::npos;
}

void RejectsWithoutChangingOutput(const RtpCodecParameters& remote,
                                 const RtpCodecParameters& local)
{
    RtpCodecParameters answer{120, "sentinel", 1234, 3, "sentinel", {{"sentinel", "value"}}};
    CHECK(!NegotiateRtpCodec(remote, local, answer));
    CHECK(answer.payloadType == 120 && answer.encodingName == "sentinel");
    CHECK(answer.clockRate == 1234 && answer.channels == 3 && answer.fmtp == "sentinel");
    CHECK(answer.rtcpFeedback.size() == 1);
    CHECK(answer.rtcpFeedback[0].type == "sentinel" && answer.rtcpFeedback[0].parameter == "value");
}

void H264LevelsAndAsymmetry()
{
    auto remote = H264(" LEVEL-ASYMMETRY-ALLOWED = 0 ; PACKETIZATION-MODE = 1 ; PROFILE-LEVEL-ID = 42E02A ; ");
    auto local = H264("profile-level-id=42e01f;packetization-mode=1;level-asymmetry-allowed=1");
    local.payloadType = 100;
    RtpCodecParameters answer;
    CHECK(NegotiateRtpCodec(remote, local, answer));
    CHECK(answer.payloadType == 96 && answer.rtcpFeedback.empty());
    CHECK(Has(answer, "profile-level-id=42e01f") && Has(answer, "level-asymmetry-allowed=0"));

    // RFC 6184 section 8.2.2 permits the local receive level only when
    // both endpoints advertise level-asymmetry-allowed=1.
    remote.fmtp = "profile-level-id=42e015;packetization-mode=1;level-asymmetry-allowed=1";
    CHECK(NegotiateRtpCodec(remote, local, answer));
    CHECK(Has(answer, "profile-level-id=42e01f") && Has(answer, "level-asymmetry-allowed=1"));
    local.fmtp = "profile-level-id=42e01f;packetization-mode=1";
    CHECK(NegotiateRtpCodec(remote, local, answer));
    CHECK(Has(answer, "profile-level-id=42e015") && Has(answer, "level-asymmetry-allowed=0"));
    remote.fmtp = "profile-level-id=42e015;packetization-mode=1";
    local.fmtp += ";level-asymmetry-allowed=1";
    CHECK(NegotiateRtpCodec(remote, local, answer));
    CHECK(Has(answer, "profile-level-id=42e015") && Has(answer, "level-asymmetry-allowed=0"));
}

void H264DefaultsAndProfilePatterns()
{
    RtpCodecParameters answer;
    CHECK(NegotiateRtpCodec(H264(""), H264(""), answer) && answer.fmtp.empty());
    // This project's omitted-profile convention is WebRTC's 42e01f,
    // rather than the different Baseline Level 1 default in RFC 6184.
    CHECK(NegotiateRtpCodec(H264(""), H264("profile-level-id=42e01f"), answer));
    CHECK(Has(answer, "profile-level-id=42e01f"));
    CHECK(NegotiateRtpCodec(H264("packetization-mode=0"), H264(""), answer));
    CHECK(Has(answer, "packetization-mode=0"));

    // Table 5: these different profile_idc/profile-iop pairs represent
    // the same sub-profile, so string equality would reject valid offers.
    for (const auto* offered : {"4de01f", "58c01f", "42c01f"})
    {
        CHECK(NegotiateRtpCodec(H264(std::string("profile-level-id=") + offered),
                               H264("profile-level-id=42e01f"), answer));
        CHECK(Has(answer, std::string("profile-level-id=") + offered));
    }
    CHECK(NegotiateRtpCodec(H264("profile-level-id=58801f"), H264("profile-level-id=42001f"), answer));
    CHECK(NegotiateRtpCodec(H264("profile-level-id=4d401f"), H264("profile-level-id=4d001f"), answer));
    CHECK(NegotiateRtpCodec(H264("profile-level-id=58001f"), H264("profile-level-id=58001e"), answer));
    CHECK(NegotiateRtpCodec(H264("profile-level-id=640c1f"), H264("profile-level-id=640c1e"), answer));
    RejectsWithoutChangingOutput(H264("profile-level-id=640c1f"), H264("profile-level-id=42e01f"));
    RejectsWithoutChangingOutput(H264("profile-level-id=640c1f"), H264("profile-level-id=64001f"));
}

void H264Level1b()
{
    RtpCodecParameters answer;
    // RFC 6184 section 8.2.2 examples downgrade 42A00B (Level 1.1)
    // to 42B00B (Level 1b): the middle byte matters as well as level_idc.
    CHECK(NegotiateRtpCodec(H264("profile-level-id=42a00b"), H264("profile-level-id=42b00b"), answer));
    CHECK(Has(answer, "profile-level-id=42b00b"));
    CHECK(NegotiateRtpCodec(H264("profile-level-id=42e00b"), H264("profile-level-id=42f00b"), answer));
    CHECK(Has(answer, "profile-level-id=42f00b"));
    CHECK(NegotiateRtpCodec(H264("profile-level-id=42f00b"), H264("profile-level-id=42e00a"), answer));
    CHECK(Has(answer, "profile-level-id=42e00a"));
    CHECK(NegotiateRtpCodec(H264("profile-level-id=4d100b"), H264("profile-level-id=4d001f"), answer));
    CHECK(Has(answer, "profile-level-id=4d100b"));

    // Non-Baseline/Main/Extended profiles encode 1b as level_idc=9,
    // whose semantic order is still Level 1 < Level 1b < Level 1.1.
    CHECK(NegotiateRtpCodec(H264("profile-level-id=64000b"), H264("profile-level-id=640009"), answer));
    CHECK(Has(answer, "profile-level-id=640009"));
    CHECK(NegotiateRtpCodec(H264("profile-level-id=640009"), H264("profile-level-id=64000a"), answer));
    CHECK(Has(answer, "profile-level-id=64000a"));
}

void H264RejectsMalformedAndUnsupportedParameters()
{
    const std::string unsupported[] = {
        "profile-level-id=42e11f", // reserved bits
        "profile-level-id=42e000", "profile-level-id=42e00f", // undefined level
        "profile-level-id=42e009", // wrong Level 1b encoding for Baseline
        "profile-level-id=+2e01f", "profile-level-id=42e01", "profile-level-id=000000",
        "profile-level-id=42e01g", "profile-level-id=", "profile-level-id",
        "packetization-mode=2", "packetization-mode=01", "packetization-mode=1", // default peer mode is 0
        "level-asymmetry-allowed=2", "level-asymmetry-allowed=01",
        "profile-level-id=42e01f;profile-level-id=42e01f",
        "profile-level-id=42e01f;PROFILE-LEVEL-ID=42e01f",
        "profile-level-id=42e01f;max-fs=100", // not an implemented encoding constraint
        "profile-level-id=42e01f;max-recv-level=0032",
        "profile-level-id=42e01f;sprop-parameter-sets=AA==,BB==",
        "profile-level-id=42e01f\na=x", "profile-level-id=42e01f;;packetization-mode=0"
    };
    for (const auto& parameters : unsupported)
    {
        RejectsWithoutChangingOutput(H264(parameters), H264(""));
        RejectsWithoutChangingOutput(H264(""), H264(parameters));
    }
    RejectsWithoutChangingOutput(H264("packetization-mode=2"), H264("packetization-mode=2"));
}

void OpusParametersAreDirectional()
{
    RtpCodecParameters remote{111, "OPUS", 48000, 2,
        "minptime=20;stereo=1;useinbandfec=0;maxaveragebitrate=32000;maxplaybackrate=16000;"
        "sprop-stereo=1;sprop-maxcapturerate=24000;x-vendor=2", {}};
    RtpCodecParameters local{112, "opus", 48000, 2,
        "minptime=10;stereo=0;useinbandfec=1;maxaveragebitrate=64000;maxplaybackrate=48000;"
        "sprop-stereo=0;sprop-maxcapturerate=48000;cbr=1;usedtx=0;x-vendor=1", {}};
    RtpCodecParameters answer;
    CHECK(NegotiateRtpCodec(remote, local, answer));
    CHECK(answer.payloadType == 111);
    for (const auto* parameter : {"minptime=10", "stereo=0", "useinbandfec=1",
         "maxaveragebitrate=64000", "maxplaybackrate=48000", "sprop-stereo=0",
         "sprop-maxcapturerate=48000", "cbr=1", "usedtx=0"})
        CHECK(Has(answer, parameter));
    CHECK(answer.fmtp.find("x-vendor") == std::string::npos);
    local.fmtp.clear();
    CHECK(NegotiateRtpCodec(remote, local, answer) && answer.fmtp.empty());

    remote.channels = 1;
    RejectsWithoutChangingOutput(remote, local);
    remote.channels = 2;
    remote.clockRate = local.clockRate = 16000;
    RejectsWithoutChangingOutput(remote, local);
    remote.clockRate = local.clockRate = 48000;
    for (const auto* invalid : {"stereo=2", "useinbandfec=-1", "maxplaybackrate=7999",
         "sprop-maxcapturerate=48001", "minptime=0", "maxaveragebitrate=510001",
         "maxaveragebitrate=999999999999999999999999"})
    {
        remote.fmtp = invalid;
        RejectsWithoutChangingOutput(remote, local);
    }
}

void GenericCapabilitiesAndRepairCodecs()
{
    RtpCodecParameters remote{110, "Example", 16000, 1, "b=two ; A = one", {}};
    RtpCodecParameters local{111, "example", 16000, 0, "a=one;b=two", {}};
    RtpCodecParameters answer;
    CHECK(NegotiateRtpCodec(remote, local, answer));
    CHECK(answer.payloadType == 110 && Has(answer, "a=one") && Has(answer, "b=two"));
    remote.fmtp = "b=Two;a=one"; // Unknown values retain their case-sensitive meaning.
    RejectsWithoutChangingOutput(remote, local);
    remote.fmtp = local.fmtp;
    remote.clockRate = 8000;
    RejectsWithoutChangingOutput(remote, local);
    remote.clockRate = local.clockRate;
    remote.channels = 2;
    RejectsWithoutChangingOutput(remote, local);
    remote.channels = 1;
    for (const char* name : {"RTX", "RED", "ULPFEC", "flexfec", "flexfec-03", "parityfec", "fec"})
    {
        remote.encodingName = local.encodingName = name;
        remote.fmtp = local.fmtp = "";
        RejectsWithoutChangingOutput(remote, local);
    }
    for (const auto* name : {"PCMU", "PCMA"})
    {
        RtpCodecParameters pcm{0, name, 8000, 0, "", {}};
        CHECK(NegotiateRtpCodec(pcm, pcm, answer));
        pcm.clockRate = 16000;
        RejectsWithoutChangingOutput(pcm, pcm);
    }
    RtpCodecParameters vp8{96, "VP8", 90000, 0, "max-fr=30;max-fs=3600", {}};
    remote = vp8;
    remote.fmtp = "max-fs = 3600 ; max-fr = 30";
    CHECK(NegotiateRtpCodec(remote, vp8, answer));
    remote.payloadType = 128;
    RejectsWithoutChangingOutput(remote, vp8);
}
} // namespace

int main()
{
    try
    {
        H264LevelsAndAsymmetry();
        H264DefaultsAndProfilePatterns();
        H264Level1b();
        H264RejectsMalformedAndUnsupportedParameters();
        OpusParametersAreDirectional();
        GenericCapabilitiesAndRepairCodecs();
        std::cout << "WebRTC codec negotiation tests passed\n";
        return 0;
    }
    catch (const std::exception& error)
    {
        std::cerr << "WebRTC codec negotiation test failed: " << error.what() << '\n';
        return 1;
    }
}
