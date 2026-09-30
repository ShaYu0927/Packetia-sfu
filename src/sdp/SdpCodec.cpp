#include "SdpCodec.h"
#include "../../utils/StringUtil.h"

#include <algorithm>
#include <array>
#include <iterator>
#include <map>
#include <string_view>
#include <utility>
#include <stdexcept>
#include <string>

namespace sdp
{
namespace
{

void Require(bool condition, const char* message)
{
    if (!condition) throw std::invalid_argument(message);
}

RtpCodecParameters Codec(int payloadType, const char* name, int clockRate, int channels)
{
    Require(payloadType >= 0 && payloadType <= 127, "RTP payload type must be between 0 and 127");
    RtpCodecParameters codec;
    codec.payloadType = payloadType;
    codec.encodingName = name;
    codec.clockRate = clockRate;
    codec.channels = channels;
    return codec;
}

void Parameter(std::string& fmtp, const char* name, const std::string& value)
{
    if (!fmtp.empty()) fmtp += ';';
    fmtp += name;
    fmtp += '=';
    fmtp += value;
}

void Parameter(std::string& fmtp, const char* name, int value)
{
    Parameter(fmtp, name, std::to_string(value));
}

void OptionalNumber(std::string& fmtp, const char* name, const std::optional<int>& value,
                    int minimum, int maximum)
{
    if (!value) return;
    Require(*value >= minimum && *value <= maximum, "Codec fmtp number is out of range");
    Parameter(fmtp, name, *value);
}

void OptionalFlag(std::string& fmtp, const char* name, const std::optional<bool>& value)
{
    if (value) Parameter(fmtp, name, *value ? 1 : 0);
}

std::string Hex(uint32_t value, size_t width)
{
    static constexpr char digits[] = "0123456789abcdef";
    std::string result(width, '0');
    for (size_t i = width; i > 0; --i)
    {
        result[i - 1] = digits[value & 15];
        value >>= 4;
    }
    return result;
}

std::string Base64(const std::vector<uint8_t>& bytes)
{
    static constexpr char alphabet[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string result;
    for (size_t offset = 0; offset < bytes.size(); offset += 3)
    {
        const auto remaining = bytes.size() - offset;
        const uint32_t bits = (uint32_t(bytes[offset]) << 16) |
            (remaining > 1 ? uint32_t(bytes[offset + 1]) << 8 : 0) |
            (remaining > 2 ? uint32_t(bytes[offset + 2]) : 0);
        result += alphabet[(bits >> 18) & 63];
        result += alphabet[(bits >> 12) & 63];
        result += remaining > 1 ? alphabet[(bits >> 6) & 63] : '=';
        result += remaining > 2 ? alphabet[bits & 63] : '=';
    }
    return result;
}

bool HasAnnexBPrefix(const std::vector<uint8_t>& nal)
{
    return nal.size() >= 3 && nal[0] == 0 && nal[1] == 0 &&
        (nal[2] == 1 || (nal.size() >= 4 && nal[2] == 0 && nal[3] == 1));
}

void H264Nal(const std::vector<uint8_t>& nal, uint8_t type, size_t minimum)
{
    Require(nal.size() >= minimum && !HasAnnexBPrefix(nal) &&
        (nal[0] & 0x80) == 0 && (nal[0] & 0x60) != 0 && (nal[0] & 31) == type,
        "Expected a raw H264 parameter-set NAL without an Annex-B prefix");
}

bool ValidH264ProfileLevel(uint32_t profileLevelId)
{
    if (profileLevelId > 0xffffff || (profileLevelId >> 16) == 0 ||
        ((profileLevelId >> 8) & 3) != 0) return false;
    switch (profileLevelId & 0xff)
    {
    case 9: // Baseline/Main/Extended signal Level 1b through constraint_set3.
        return (profileLevelId >> 16) != 66 && (profileLevelId >> 16) != 77 && (profileLevelId >> 16) != 88;
    case 10: case 11: case 12: case 13:
    case 20: case 21: case 22:
    case 30: case 31: case 32:
    case 40: case 41: case 42:
    case 50: case 51: case 52:
    case 60: case 61: case 62:
        return true;
    default:
        return false;
    }
}

void H265ParameterSet(std::string& fmtp, const char* name,
                      const std::vector<uint8_t>& nal, uint8_t type)
{
    if (nal.empty()) return;
    Require(nal.size() >= 2 && !HasAnnexBPrefix(nal) && (nal[0] & 0x80) == 0 &&
        ((nal[0] >> 1) & 63) == type && (nal[1] & 7) != 0,
        "Expected a raw H265 parameter-set NAL without an Annex-B prefix");
    Parameter(fmtp, name, Base64(nal));
}

using Parameters = std::map<std::string, std::string>;

bool ParseParameters(const std::string& fmtp, Parameters& result)
{
    // Reject embedded SDP line breaks before trimming whitespace.
    for (unsigned char c : fmtp)
        if ((c < 0x20 && c != '\t') || c == 0x7f) return false;

    std::string_view remaining = utils::TrimSpaceAndTab(fmtp);
    while (!remaining.empty())
    {
        const auto separator = remaining.find(';');
        const auto parameter = utils::TrimSpaceAndTab(remaining.substr(0, separator));
        const auto equals = parameter.find('=');
        if (equals == std::string_view::npos) return false;
        const auto key = utils::TrimSpaceAndTab(parameter.substr(0, equals));
        const auto value = utils::TrimSpaceAndTab(parameter.substr(equals + 1));
        if (key.empty() || value.empty() ||
            !std::all_of(key.begin(), key.end(), utils::IsTokenCharacter)) return false;
        if (!result.emplace(utils::ToLowerAscii(std::string(key)), std::string(value)).second)
            return false;
        if (separator == std::string_view::npos) break;
        remaining = utils::TrimSpaceAndTab(remaining.substr(separator + 1));
    }
    return true;
}

std::string Serialize(const Parameters& parameters)
{
    std::string result;
    for (const auto& parameter : parameters)
    {
        if (!result.empty()) result += ';';
        result += parameter.first;
        result += '=';
        result += parameter.second;
    }
    return result;
}

bool ParseNumber(const std::string& value, uint32_t minimum, uint32_t maximum,
                 uint32_t& result)
{
    if (value.empty()) return false;
    uint32_t number = 0;
    for (char c : value)
    {
        if (c < '0' || c > '9') return false;
        const auto digit = static_cast<uint32_t>(c - '0');
        if (number > maximum / 10 ||
            (number == maximum / 10 && digit > maximum % 10)) return false;
        number = number * 10 + digit;
    }
    if (number < minimum) return false;
    result = number;
    return true;
}

bool BooleanParameter(const Parameters& parameters, const char* key, bool& result)
{
    const auto it = parameters.find(key);
    if (it == parameters.end()) { result = false; return true; }
    if (it->second != "0" && it->second != "1") return false;
    result = it->second == "1";
    return true;
}

enum class H264Profile { ConstrainedBaseline, Baseline, Main, Extended, High, ConstrainedHigh };

struct ProfilePattern
{
    uint8_t idc;
    uint8_t mask;
    uint8_t value;
    H264Profile profile;
};

// RFC 6184 Table 5 equivalence patterns, plus Constrained High used by
// WebRTC. Other H.264 sub-profiles require explicit support before accepting.
constexpr ProfilePattern kProfiles[] = {
    {0x42, 0x4f, 0x40, H264Profile::ConstrainedBaseline},
    {0x4d, 0x8f, 0x80, H264Profile::ConstrainedBaseline},
    {0x58, 0xcf, 0xc0, H264Profile::ConstrainedBaseline},
    {0x42, 0x4f, 0x00, H264Profile::Baseline},
    {0x58, 0xcf, 0x80, H264Profile::Baseline},
    {0x4d, 0xaf, 0x00, H264Profile::Main},
    {0x58, 0xcf, 0x00, H264Profile::Extended},
    {0x64, 0xff, 0x00, H264Profile::High},
    {0x64, 0xff, 0x0c, H264Profile::ConstrainedHigh},
};

struct H264ProfileLevel
{
    H264Profile profile;
    uint8_t idc;
    uint8_t iop;
    // Ordinary levels use level_idc * 10; 105 places 1b between 1 and 1.1.
    int level;
};

bool ParseH264ProfileLevel(const Parameters& parameters, H264ProfileLevel& result)
{
    const auto parameter = parameters.find("profile-level-id");
    const std::string_view value = parameter == parameters.end()
        ? std::string_view("42e01f") : std::string_view(parameter->second);
    if (value.size() != 6) return false;
    uint32_t bits = 0;
    for (char c : value)
    {
        int digit;
        if (c >= '0' && c <= '9') digit = c - '0';
        else if (c >= 'a' && c <= 'f') digit = c - 'a' + 10;
        else if (c >= 'A' && c <= 'F') digit = c - 'A' + 10;
        else return false;
        bits = (bits << 4) | static_cast<uint32_t>(digit);
    }
    const auto idc = static_cast<uint8_t>(bits >> 16);
    const auto iop = static_cast<uint8_t>(bits >> 8);
    const auto levelIdc = static_cast<uint8_t>(bits);
    const auto profile = std::find_if(std::begin(kProfiles), std::end(kProfiles),
        [=](const auto& pattern) { return idc == pattern.idc && (iop & pattern.mask) == pattern.value; });
    if (profile == std::end(kProfiles)) return false;

    const bool legacyLevel1b = idc == 0x42 || idc == 0x4d || idc == 0x58;
    int level;
    if ((legacyLevel1b && levelIdc == 11 && (iop & 0x10) != 0) ||
        (!legacyLevel1b && levelIdc == 9))
        level = 105;
    else
    {
        switch (levelIdc)
        {
        case 10: case 11: case 12: case 13:
        case 20: case 21: case 22:
        case 30: case 31: case 32:
        case 40: case 41: case 42:
        case 50: case 51: case 52:
        case 60: case 61: case 62:
            level = levelIdc * 10;
            break;
        default: return false;
        }
    }
    result = {profile->profile, idc, iop, level};
    return true;
}

std::string H264ProfileLevelString(const H264ProfileLevel& offered, int level)
{
    auto iop = offered.iop;
    uint8_t levelIdc = static_cast<uint8_t>(level / 10);
    if (offered.idc == 0x42 || offered.idc == 0x4d || offered.idc == 0x58)
    {
        iop &= static_cast<uint8_t>(~0x10);
        if (level == 105) { iop |= 0x10; levelIdc = 11; }
    }
    else if (level == 105) levelIdc = 9;

    constexpr char hex[] = "0123456789abcdef";
    std::string result;
    for (uint8_t byte : {offered.idc, iop, levelIdc})
    {
        result += hex[byte >> 4];
        result += hex[byte & 0x0f];
    }
    return result;
}

bool NegotiateH264(const Parameters& remote, const Parameters& local, Parameters& answer)
{
    for (const auto* parameters : {&remote, &local})
        for (const auto& parameter : *parameters)
            if (parameter.first != "profile-level-id" &&
                parameter.first != "packetization-mode" &&
                parameter.first != "level-asymmetry-allowed") return false;

    bool remoteMode, localMode, remoteAsymmetry, localAsymmetry;
    if (!BooleanParameter(remote, "packetization-mode", remoteMode) ||
        !BooleanParameter(local, "packetization-mode", localMode) || remoteMode != localMode ||
        !BooleanParameter(remote, "level-asymmetry-allowed", remoteAsymmetry) ||
        !BooleanParameter(local, "level-asymmetry-allowed", localAsymmetry)) return false;

    H264ProfileLevel remoteProfile, localProfile;
    if (!ParseH264ProfileLevel(remote, remoteProfile) ||
        !ParseH264ProfileLevel(local, localProfile) || remoteProfile.profile != localProfile.profile)
        return false;

    const bool asymmetry = remoteAsymmetry && localAsymmetry;
    const int level = asymmetry ? localProfile.level : std::min(remoteProfile.level, localProfile.level);
    if (remote.count("profile-level-id") || local.count("profile-level-id"))
        answer["profile-level-id"] = H264ProfileLevelString(remoteProfile, level);
    if (remote.count("packetization-mode") || local.count("packetization-mode"))
        answer["packetization-mode"] = remoteMode ? "1" : "0";
    if (remote.count("level-asymmetry-allowed") || local.count("level-asymmetry-allowed"))
        answer["level-asymmetry-allowed"] = asymmetry ? "1" : "0";
    return true;
}

bool OpusParameters(const Parameters& parameters, Parameters& supported)
{
    for (const auto& parameter : parameters)
    {
        const auto& key = parameter.first;
        uint32_t minimum, maximum;
        if (key == "stereo" || key == "sprop-stereo" || key == "cbr" ||
            key == "useinbandfec" || key == "usedtx")
        {
            if (parameter.second != "0" && parameter.second != "1") return false;
            supported.emplace(parameter);
            continue;
        }
        if (key == "maxplaybackrate" || key == "sprop-maxcapturerate")
        { minimum = 8000; maximum = 48000; }
        else if (key == "maxaveragebitrate")
        { minimum = 6000; maximum = 510000; }
        else if (key == "minptime")
        { minimum = 3; maximum = 120; }
        else continue; // RFC 7587: ignore unknown offer parameters; do not echo.

        uint32_t number;
        if (!ParseNumber(parameter.second, minimum, maximum, number)) return false;
        supported.emplace(key, std::to_string(number));
    }
    return true;
}

bool IsRepairCodec(const std::string& name)
{
    return name == "rtx" || name == "red" || name == "ulpfec" || name == "flexfec" ||
        name == "flexfec-03" || name == "parityfec" || name == "fec";
}
} // namespace

bool SdpCodec::Negotiate(const RtpCodecParameters& remote, const RtpCodecParameters& local, RtpCodecParameters& negotiated)
{
    const auto name = utils::ToLowerAscii(local.encodingName);
    if (name.empty() || !std::all_of(name.begin(), name.end(), utils::IsTokenCharacter) ||
        name != utils::ToLowerAscii(remote.encodingName) || IsRepairCodec(name) ||
        remote.payloadType < 0 || remote.payloadType > 127 ||
        local.clockRate <= 0 || remote.clockRate != local.clockRate ||
        local.channels < 0 || remote.channels < 0 ||
        std::max(1, remote.channels) != std::max(1, local.channels)) return false;

    Parameters remoteParameters, localParameters, answer;
    if (!ParseParameters(remote.fmtp, remoteParameters) || !ParseParameters(local.fmtp, localParameters)) return false;

    if (name == "h264")
    {
        if (local.clockRate != 90000 || local.channels > 1 ||
            !NegotiateH264(remoteParameters, localParameters, answer)) return false;
    }
    else if (name == "opus")
    {
        Parameters remoteSupported;
        if (local.clockRate != 48000 || local.channels != 2 ||
            !OpusParameters(remoteParameters, remoteSupported) ||
            !OpusParameters(localParameters, answer)) return false;
    }
    else
    {
        if ((name == "pcmu" || name == "pcma") &&
            (local.clockRate != 8000 || local.channels > 1)) return false;
        if (name == "vp8" && (local.clockRate != 90000 || local.channels > 1)) return false;
        if (remoteParameters != localParameters) return false;
        answer = std::move(localParameters);
    }

    RtpCodecParameters result = local;
    result.payloadType = remote.payloadType;
    result.fmtp = Serialize(answer);
    result.rtcpFeedback.clear();
    negotiated = std::move(result);
    return true;
}

bool SdpCodec::IsAnswer(const RtpCodecParameters& offer, const RtpCodecParameters& answer)
{
    if (offer.payloadType != answer.payloadType) return false;
    RtpCodecParameters negotiated;
    if (!Negotiate(offer, answer, negotiated)) return false;
    Parameters offered, accepted;
    if (!ParseParameters(offer.fmtp, offered) || !ParseParameters(answer.fmtp, accepted)) return false;
    const auto name = utils::ToLowerAscii(offer.encodingName);
    if (name == "h264")
    {
        bool offerAsymmetry, answerAsymmetry;
        H264ProfileLevel offerProfile, answerProfile;
        if (!BooleanParameter(offered, "level-asymmetry-allowed", offerAsymmetry) ||
            !BooleanParameter(accepted, "level-asymmetry-allowed", answerAsymmetry) ||
            !ParseH264ProfileLevel(offered, offerProfile) || !ParseH264ProfileLevel(accepted, answerProfile)) return false;
        return (!answerAsymmetry || offerAsymmetry) && ((offerAsymmetry && answerAsymmetry) || answerProfile.level <= offerProfile.level);
    }
    if (name == "opus")
    {
        Parameters supported;
        if (!OpusParameters(accepted, supported)) return false;
        for (const auto& parameter : accepted)
        {
            if (supported.count(parameter.first)) continue;
            const auto found = offered.find(parameter.first);
            if (found == offered.end() || found->second != parameter.second) return false;
        }
    }
    return true;
}

RtpCodecParameters SdpCodec::H264(int payloadType, const H264CodecConfig& config)
{
    auto codec = Codec(payloadType, "H264", 90000, 0);
    Require(config.packetizationMode == 0 || config.packetizationMode == 1, "H264 packetization mode must be 0 or 1");
    Require(config.sps.empty() == config.pps.empty(), "H264 SPS and PPS must be supplied together");
    uint32_t profile = config.profileLevelId;
    if (!config.sps.empty())
    {
        H264Nal(config.sps, 7, 4);
        H264Nal(config.pps, 8, 1);
        profile = (uint32_t(config.sps[1]) << 16) | (uint32_t(config.sps[2]) << 8) | config.sps[3];
    }
    Require(ValidH264ProfileLevel(profile), "Invalid H264 profile-level-id, reserved bits or level");
    Parameter(codec.fmtp, "packetization-mode", config.packetizationMode);
    Parameter(codec.fmtp, "profile-level-id", Hex(profile, 6));
    Parameter(codec.fmtp, "level-asymmetry-allowed", config.levelAsymmetryAllowed ? 1 : 0);
    if (!config.sps.empty())
        Parameter(codec.fmtp, "sprop-parameter-sets", Base64(config.sps) + "," + Base64(config.pps));
    return codec;
}

RtpCodecParameters SdpCodec::H265(int payloadType, const H265CodecConfig& config)
{
    auto codec = Codec(payloadType, "H265", 90000, 0);
    Require(config.profileSpace >= 0 && config.profileSpace <= 3, "H265 profile-space must be between 0 and 3");
    Require(config.profileId >= 0 && config.profileId <= 31, "H265 profile-id must be between 0 and 31");
    Require(config.levelId >= 0 && config.levelId <= 255, "H265 level-id must be between 0 and 255");
    Require(!config.tierFlag || config.levelId >= 120, "H265 high tier requires Level 4 or higher");
    Parameter(codec.fmtp, "profile-space", config.profileSpace);
    Parameter(codec.fmtp, "profile-id", config.profileId);
    Parameter(codec.fmtp, "tier-flag", config.tierFlag ? 1 : 0);
    Parameter(codec.fmtp, "level-id", config.levelId);
    H265ParameterSet(codec.fmtp, "sprop-vps", config.vps, 32);
    H265ParameterSet(codec.fmtp, "sprop-sps", config.sps, 33);
    H265ParameterSet(codec.fmtp, "sprop-pps", config.pps, 34);
    return codec;
}

RtpCodecParameters SdpCodec::Opus(int payloadType, const OpusCodecConfig& config)
{
    auto codec = Codec(payloadType, "opus", 48000, 2);
    OptionalNumber(codec.fmtp, "maxaveragebitrate", config.maxAverageBitrate, 6000, 510000);
    OptionalNumber(codec.fmtp, "maxplaybackrate", config.maxPlaybackRate, 8000, 48000);
    OptionalNumber(codec.fmtp, "sprop-maxcapturerate", config.spropMaxCaptureRate, 8000, 48000);
    OptionalNumber(codec.fmtp, "minptime", config.minPtime, 3, 120);
    OptionalFlag(codec.fmtp, "stereo", config.stereo);
    OptionalFlag(codec.fmtp, "sprop-stereo", config.spropStereo);
    OptionalFlag(codec.fmtp, "useinbandfec", config.useInbandFec);
    OptionalFlag(codec.fmtp, "usedtx", config.useDtx);
    OptionalFlag(codec.fmtp, "cbr", config.cbr);
    return codec;
}

RtpCodecParameters SdpCodec::Aac(int payloadType, const AacCodecConfig& config)
{
    static constexpr std::array<int, 13> frequencies{
        96000, 88200, 64000, 48000, 44100, 32000, 24000,
        22050, 16000, 12000, 11025, 8000, 7350};
    const auto frequency = std::find(frequencies.begin(), frequencies.end(), config.sampleRate);
    Require(frequency != frequencies.end(), "AAC-LC requires an indexed MPEG-4 sampling frequency");
    Require((config.channels >= 1 && config.channels <= 6) || config.channels == 8,
        "AAC-LC supports 1..6 or 8 channels without a program configuration element");
    auto codec = Codec(payloadType, "MPEG4-GENERIC", config.sampleRate, config.channels);
    // audioObjectType=2 (AAC-LC), samplingFrequencyIndex, channelConfiguration,
    // and GASpecificConfig flags all zero: 1024-sample frames, no extensions.
    const auto frequencyIndex = static_cast<uint32_t>(frequency - frequencies.begin());
    const auto channelConfiguration = static_cast<uint32_t>(config.channels == 8 ? 7 : config.channels);
    const uint32_t asc = (2U << 11) | (frequencyIndex << 7) | (channelConfiguration << 3);
    // MPEG-4 Audio Profile Level indication, not the audioObjectType. AAC
    // levels 4/5 allow five main channels plus LFE; levels 6/7 allow seven
    // plus LFE. Levels 6/7 use 0x50/0x51, not the HE-AAC values 0x2c/0x2d.
    int profileLevel;
    if (config.channels <= 2 && config.sampleRate <= 24000) profileLevel = 40;
    else if (config.channels <= 2 && config.sampleRate <= 48000) profileLevel = 41;
    else if (config.channels <= 6 && config.sampleRate <= 48000) profileLevel = 42;
    else if (config.channels <= 6) profileLevel = 43;
    else if (config.sampleRate <= 48000) profileLevel = 80;
    else profileLevel = 81;
    Parameter(codec.fmtp, "streamtype", 5);
    Parameter(codec.fmtp, "profile-level-id", profileLevel);
    Parameter(codec.fmtp, "mode", std::string("AAC-hbr"));
    Parameter(codec.fmtp, "config", Hex(asc, 4));
    // RFC 3640 section 3.3.6 defines a 16-bit AU header for AAC-hbr.
    Parameter(codec.fmtp, "sizeLength", 13);
    Parameter(codec.fmtp, "indexLength", 3);
    Parameter(codec.fmtp, "indexDeltaLength", 3);
    return codec;
}

RtpCodecParameters SdpCodec::AmrWb(int payloadType, const AmrWbCodecConfig& config)
{
    Require(config.channels >= 1 && config.channels <= 6, "AMR-WB channel count must be between 1 and 6");
    Require(!config.interleaving || *config.interleaving > 0, "AMR-WB interleaving must be positive");
    Require(config.octetAlign || (!config.crc.value_or(false) && !config.robustSorting.value_or(false) &&
        !config.interleaving), "AMR-WB CRC, robust sorting and interleaving require octet alignment");
    auto codec = Codec(payloadType, "AMR-WB", 16000, config.channels);
    Parameter(codec.fmtp, "octet-align", config.octetAlign ? 1 : 0);
    std::array<bool, 9> modes{};
    std::string modeSet;
    for (const int mode : config.modeSet)
    {
        Require(mode >= 0 && mode <= 8, "AMR-WB mode-set entries must be between 0 and 8");
        Require(!modes[mode], "AMR-WB mode-set must not contain duplicates");
        modes[mode] = true;
        if (!modeSet.empty()) modeSet += ',';
        modeSet += std::to_string(mode);
    }
    if (!modeSet.empty()) Parameter(codec.fmtp, "mode-set", modeSet);
    OptionalFlag(codec.fmtp, "crc", config.crc);
    OptionalFlag(codec.fmtp, "robust-sorting", config.robustSorting);
    if (config.interleaving) Parameter(codec.fmtp, "interleaving", *config.interleaving);
    return codec;
}

} // namespace sdp
