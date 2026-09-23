#include "WebRtcCodec.h"

#include <algorithm>
#include <cstdint>
#include <iterator>
#include <map>
#include <string>
#include <string_view>
#include <utility>

namespace protocol::webrtc
{
namespace
{
using Parameters = std::map<std::string, std::string>;

std::string Lower(std::string value)
{
    for (char& c : value)
        if (c >= 'A' && c <= 'Z') c = static_cast<char>(c + ('a' - 'A'));
    return value;
}

std::string_view Trim(std::string_view value)
{
    const auto begin = value.find_first_not_of(" \t");
    if (begin == std::string_view::npos) return {};
    const auto end = value.find_last_not_of(" \t");
    return value.substr(begin, end - begin + 1);
}

bool IsTokenCharacter(char c)
{
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
        (c >= '0' && c <= '9') ||
        std::string_view("!#$%&'*+-.^_`|~").find(c) != std::string_view::npos;
}

bool ParseParameters(const std::string& fmtp, Parameters& result)
{
    // Reject embedded SDP line breaks before trimming whitespace.
    for (unsigned char c : fmtp)
        if ((c < 0x20 && c != '\t') || c == 0x7f) return false;

    std::string_view remaining = Trim(fmtp);
    while (!remaining.empty())
    {
        const auto separator = remaining.find(';');
        const auto parameter = Trim(remaining.substr(0, separator));
        const auto equals = parameter.find('=');
        if (equals == std::string_view::npos) return false;
        const auto key = Trim(parameter.substr(0, equals));
        const auto value = Trim(parameter.substr(equals + 1));
        if (key.empty() || value.empty() ||
            !std::all_of(key.begin(), key.end(), IsTokenCharacter)) return false;
        if (!result.emplace(Lower(std::string(key)), std::string(value)).second)
            return false;
        if (separator == std::string_view::npos) break;
        remaining = Trim(remaining.substr(separator + 1));
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

bool NegotiateRtpCodec(const RtpCodecParameters& remote,
                       const RtpCodecParameters& local,
                       RtpCodecParameters& negotiated)
{
    const auto name = Lower(local.encodingName);
    if (name.empty() || !std::all_of(name.begin(), name.end(), IsTokenCharacter) ||
        name != Lower(remote.encodingName) || IsRepairCodec(name) ||
        remote.payloadType < 0 || remote.payloadType > 127 ||
        local.clockRate <= 0 || remote.clockRate != local.clockRate ||
        local.channels < 0 || remote.channels < 0 ||
        std::max(1, remote.channels) != std::max(1, local.channels)) return false;

    Parameters remoteParameters, localParameters, answer;
    if (!ParseParameters(remote.fmtp, remoteParameters) ||
        !ParseParameters(local.fmtp, localParameters)) return false;

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
} // namespace protocol::webrtc
