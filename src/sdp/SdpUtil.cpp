#include "SdpUtil.h"

#include <algorithm>
#include <charconv>
#include <cstdint>
#include <limits>
#include <sstream>
#include <unordered_set>

namespace sdp
{
namespace
{
std::vector<std::string> SplitBySpace(const std::string& text)
{
    std::vector<std::string> result;
    std::istringstream input(text);
    for (std::string token; input >> token;) result.push_back(std::move(token));
    return result;
}

bool Number(const std::string& text, uint64_t maximum, uint64_t& result)
{
    if (text.empty() || text.front() < '0' || text.front() > '9') return false;
    const auto parsed = std::from_chars(text.data(), text.data() + text.size(), result);
    return parsed.ec == std::errc{} && parsed.ptr == text.data() + text.size() && result <= maximum;
}

std::string MakeLineError(const char* field, const SdpLine& line, const std::string& detail)
{
    return "parse " + std::string(field) + " failed at line " + std::to_string(line.line_no)
        + ": " + detail + ", raw=\"" + line.raw + "\"";
}
} // namespace

bool SdpParser::Parse(const std::string& text, SdpSession& session, std::string& err)
{
    err.clear();
    auto lines = SplitLines(text);
    if (lines.empty()) { err = "sdp text is empty"; return false; }
    SdpSession parsed;
    if (!ParseLines(lines, parsed, err)) return false;
    session = std::move(parsed);
    return true;
}

std::vector<SdpLine> SdpParser::SplitLines(const std::string& text)
{
    std::vector<SdpLine> lines;
    std::istringstream input(text);
    int number = 0;
    for (std::string raw; std::getline(input, raw);)
    {
        ++number;
        if (!raw.empty() && raw.back() == '\r') raw.pop_back();
        if (raw.empty()) continue;
        SdpLine line;
        line.line_no = number;
        line.raw = raw;
        line.value = raw;
        if (raw.size() >= 2 && raw[1] == '='
            && std::none_of(raw.begin(), raw.end(), [](unsigned char ch) {
                return (ch < 32 && ch != '\t') || ch == 127;
            }))
        {
            line.type = raw[0];
            line.value = raw.substr(2);
        }
        lines.push_back(std::move(line));
    }
    return lines;
}

bool SdpParser::ParseLines(const std::vector<SdpLine>& lines, SdpSession& session, std::string& err)
{
    SdpMedia* current_media = nullptr;
    std::unordered_set<char> session_fields;
    for (const auto& line : lines)
    {
        if (line.type == 'v' || line.type == 'o' || line.type == 's' || line.type == 't')
        {
            if (current_media || !session_fields.insert(line.type).second)
            {
                err = MakeLineError("session", line, "duplicate or misplaced session field");
                return false;
            }
        }
        switch (line.type)
        {
        case 'v': if (!ParseVersion(line, session, err)) return false; break;
        case 'o': if (!ParseOrigin(line, session, err)) return false; break;
        case 's': if (!ParseSessionName(line, session, err)) return false; break;
        case 'c': if (!ParseConnection(line, session, current_media, err)) return false; break;
        case 't': if (!ParseTiming(line, session, err)) return false; break;
        case 'm': if (!ParseMedia(line, session, current_media, err)) return false; break;
        case 'a': if (!ParseAttribute(line, session, current_media, err)) return false; break;
        // Optional fields not represented by this model are accepted for RTSP.
        case 'i': case 'u': case 'e': case 'p': case 'b': case 'r': case 'z': case 'k': break;
        default: err = MakeLineError("SDP", line, "invalid field or missing '='"); return false;
        }
    }
    return true;
}

bool SdpParser::ParseVersion(const SdpLine& line, SdpSession& session, std::string& err)
{
    uint64_t version = 0;
    if (!Number(line.value, 0, version))
    { err = MakeLineError("v=", line, "version must be 0"); return false; }
    session.version = 0;
    return true;
}

bool SdpParser::ParseOrigin(const SdpLine& line, SdpSession& session, std::string& err)
{
    const auto parts = SplitBySpace(line.value);
    uint64_t number = 0;
    if (parts.size() != 6 || !Number(parts[1], UINT64_MAX, number) || !Number(parts[2], UINT64_MAX, number))
    { err = MakeLineError("o=", line, "expected six fields and unsigned session id/version"); return false; }
    session.origin = {parts[0], parts[1], parts[2], parts[3], parts[4], parts[5]};
    return true;
}

bool SdpParser::ParseSessionName(const SdpLine& line, SdpSession& session, std::string& err)
{
    if (line.value.empty())
    { err = MakeLineError("s=", line, "empty session name"); return false; }
    session.session_name = line.value;
    return true;
}

bool SdpParser::ParseConnection(const SdpLine& line, SdpSession& session, SdpMedia* current_media, std::string& err)
{
    const auto parts = SplitBySpace(line.value);
    auto& connection = current_media ? current_media->conn : session.conn;
    if (parts.size() != 3 || !connection.address.empty())
    { err = MakeLineError("c=", line, "invalid or duplicate connection"); return false; }
    connection = {parts[0], parts[1], parts[2]};
    if (!current_media) session.connection = line.value;
    return true;
}

bool SdpParser::ParseTiming(const SdpLine& line, SdpSession& session, std::string& err)
{
    const auto parts = SplitBySpace(line.value);
    uint64_t number = 0;
    if (parts.size() != 2 || !Number(parts[0], UINT64_MAX, number)
        || !Number(parts[1], UINT64_MAX, number))
    { err = MakeLineError("t=", line, "expected unsigned start/stop times"); return false; }
    session.timing = line.value;
    return true;
}

bool SdpParser::ParseMedia(const SdpLine& line, SdpSession& session, SdpMedia*& current_media, std::string& err)
{
    const auto parts = SplitBySpace(line.value);
    uint64_t port = 0;
    uint64_t count = 1;
    const auto slash = parts.size() >= 2 ? parts[1].find('/') : std::string::npos;
    if (parts.size() < 4 || !Number(parts[1].substr(0, slash), 65535, port)
        || (slash != std::string::npos
            && (!Number(parts[1].substr(slash + 1), 65535, count) || count == 0)))
    { err = MakeLineError("m=", line, "expected media, port in 0..65535, protocol and formats"); return false; }
    SdpMedia media;
    media.media = parts[0];
    media.port = static_cast<int>(port);
    media.portCount = static_cast<int>(count);
    media.proto = parts[2];
    media.fmts.assign(parts.begin() + 3, parts.end());
    session.medias.push_back(std::move(media));
    current_media = &session.medias.back();
    return true;
}

bool SdpParser::ParseAttribute(const SdpLine& line, SdpSession& session,SdpMedia* current_media, std::string& err)
{
    auto attribute = SplitAttribute(line.value);
    if (attribute.key.empty() || attribute.key.find_first_of(" \t") != std::string::npos)
    { err = MakeLineError("a=", line, "invalid attribute key"); return false; }
    if (current_media)
    {
        if (attribute.key == "rtpmap")
        {
            SdpRtpMap map;
            if (!ParseRtpMapValue(attribute.value, map, err))
            { err = MakeLineError("a=rtpmap", line, err); return false; }
            current_media->rtpmaps.push_back(std::move(map));
        }
        else if (attribute.key == "fmtp")
        {
            SdpFmtp format;
            if (!ParseFmtpValue(attribute.value, format, err))
            { err = MakeLineError("a=fmtp", line, err); return false; }
            current_media->fmtps.push_back(std::move(format));
        }
        current_media->attributes.push_back(std::move(attribute));
    }
    else session.attributes.push_back(std::move(attribute));
    return true;
}

SdpAttribute SdpParser::SplitAttribute(const std::string& text)
{
    const auto colon = text.find(':');
    return colon == std::string::npos ? SdpAttribute{text, {}}
        : SdpAttribute{text.substr(0, colon), text.substr(colon + 1)};
}

bool SdpParser::ParseRtpMapValue(const std::string& value, SdpRtpMap& map, std::string& err)
{
    const auto parts = SplitBySpace(value);
    uint64_t number = 0;
    if (parts.size() != 2 || !Number(parts[0], 127, number))
    { err = "invalid rtpmap payload type or fields"; return false; }
    map.payloadType = static_cast<int>(number);
    const auto first = parts[1].find('/');
    const auto second = first == std::string::npos ? first : parts[1].find('/', first + 1);
    if (first == std::string::npos || first == 0
        || (second != std::string::npos && parts[1].find('/', second + 1) != std::string::npos))
    { err = "invalid rtpmap encoding/rate/channels"; return false; }
    map.encodingName = parts[1].substr(0, first);
    const auto rate = parts[1].substr(first + 1,
        second == std::string::npos ? second : second - first - 1);
    if (!Number(rate, std::numeric_limits<int>::max(), number) || number == 0)
    { err = "invalid rtpmap clock rate"; return false; }
    map.clockRate = static_cast<int>(number);
    if (second != std::string::npos)
    {
        if (!Number(parts[1].substr(second + 1), std::numeric_limits<int>::max(), number) || number == 0)
        { err = "invalid rtpmap channels"; return false; }
        map.channels = static_cast<int>(number);
    }
    return true;
}

bool SdpParser::ParseFmtpValue(const std::string& value, SdpFmtp& format, std::string& err)
{
    const auto separator = value.find_first_of(" \t");
    uint64_t number = 0;
    if (separator == std::string::npos || !Number(value.substr(0, separator), 127, number))
    { err = "invalid fmtp payload type or missing parameters"; return false; }
    const auto params = value.find_first_not_of(" \t", separator);
    if (params == std::string::npos) { err = "empty fmtp parameters"; return false; }
    format.payloadType = static_cast<int>(number);
    format.params = value.substr(params);
    return true;
}
} // namespace sdp
