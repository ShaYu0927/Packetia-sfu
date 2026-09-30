#include "Sdp.h"
#include "SdpUtil.h"
#include "SdpWebRtc.h"

#include <algorithm>
#include <charconv>
#include <set>
#include <sstream>
#include <stdexcept>
#include <string_view>
#include <unordered_set>

namespace sdp 
{
namespace
{
bool CodecToken(const std::string& value)
{
    return !value.empty() && std::all_of(value.begin(), value.end(), [](char c)
    {
        return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
            (c >= '0' && c <= '9') || std::string_view("!#$%&'*+-.^_`|~").find(c) != std::string_view::npos;
    });
}

bool CodecAttributeValue(const std::string& value)
{
    return std::none_of(value.begin(), value.end(), [](unsigned char c)
    {
        return (c < 0x20 && c != '\t') || c == 0x7f;
    });
}

bool CodecMediaMatches(std::string name, const std::string& kind)
{
    for (auto& c : name)
        if (c >= 'A' && c <= 'Z') c = static_cast<char>(c + ('a' - 'A'));
    if (name == "h264" || name == "h265" || name == "hevc" ||
        name == "vp8" || name == "vp9" || name == "av1") return kind == "video";
    if (name == "opus" || name == "aac" ||
        name == "amr" || name == "amr-wb" || name == "pcmu" ||
        name == "pcma" || name == "g722") return kind == "audio";
    return true;
}

bool WildcardFeedback(const SdpAttribute& attribute)
{
    if (attribute.key != "rtcp-fb") return false;
    const auto start = attribute.value.find_first_not_of(" \t");
    return start != std::string::npos && attribute.value[start] == '*' &&
        (start + 1 == attribute.value.size() || attribute.value[start + 1] == ' ' ||
         attribute.value[start + 1] == '\t');
}
}

SdpParseResult Sdp::Parse(const std::string& text, SdpProfile profile)
{
    SdpParseResult result;
    std::string err;
    SdpSession tmp;

    if (!SdpParser::Parse(text, tmp, err))
    {
        result.ok = false;
        result.message = err;
        result.code = SdpErrorCode::InvalidSyntax;
        return result;
    }

    if (profile == SdpProfile::WebRtc && !detail::ParseWebRtcAttributes(text, tmp, err))
    {
        result.message = err;
        result.code = SdpErrorCode::InvalidAttribute;
        return result;
    }
    tmp.profile = profile;
    result.ok = true;
    result.session = std::move(tmp);
    return result;
}

bool Sdp::Parse(const std::string& text, SdpProfile profile, SdpType type,
    SdpSession& output, std::string& error)
{
    if (type == SdpType::Rollback)
    {
        error = "Rollback does not carry an SDP document";
        return false;
    }
    auto result = Parse(text, profile);
    if (!result.ok) { error = std::move(result.message); return false; }
    result.session.type = type;
    output = std::move(result.session);
    error.clear();
    return true;
}

void Sdp::SetCodecs(SdpMedia& media, std::vector<RtpCodecParameters> codecs)
{
    if ((media.media != "audio" && media.media != "video") ||
        (!media.proto.empty() && media.proto.find("RTP/") == std::string::npos) || codecs.empty())
        throw std::invalid_argument("RTP audio/video media requires at least one codec");
    std::set<int> payloads;
    for (auto& codec : codecs)
    {
        if (codec.payloadType < 0 || codec.payloadType > 127 || !payloads.insert(codec.payloadType).second ||
            !CodecToken(codec.encodingName) || codec.clockRate <= 0 || codec.channels < 0 ||
            (media.media == "video" && codec.channels > 1) || !CodecMediaMatches(codec.encodingName, media.media) ||
            !CodecAttributeValue(codec.fmtp) ||
            (!codec.fmtp.empty() && codec.fmtp.find_first_not_of(" \t") == std::string::npos))
            throw std::invalid_argument("Invalid RTP codec parameters or duplicate payload type");
        std::set<std::pair<std::string, std::string>> feedback;
        for (auto& item : codec.rtcpFeedback)
        {
            if (!CodecToken(item.type) || !CodecAttributeValue(item.parameter))
                throw std::invalid_argument("Invalid RTP codec feedback");
            std::istringstream input(item.parameter);
            std::string word, normalized;
            while (input >> word)
            {
                if (!normalized.empty()) normalized += ' ';
                normalized += word;
            }
            item.parameter = std::move(normalized);
            if (!feedback.emplace(item.type, item.parameter).second)
                throw std::invalid_argument("Invalid or duplicate RTP codec feedback");
        }
    }

    auto result = media;
    result.fmts.clear();
    result.rtpmaps.clear();
    result.fmtps.clear();
    auto& attributes = result.attributes;
    attributes.erase(std::remove_if(attributes.begin(), attributes.end(), [](const auto& item)
    {
        return item.key == "rtpmap" || item.key == "fmtp" ||
            (item.key == "rtcp-fb" && !WildcardFeedback(item));
    }), attributes.end());
    for (const auto& codec : codecs)
    {
        const auto payload = std::to_string(codec.payloadType);
        result.fmts.push_back(payload);
        result.rtpmaps.push_back({codec.payloadType, codec.encodingName, codec.clockRate, codec.channels});
        if (!codec.fmtp.empty()) result.fmtps.push_back({codec.payloadType, codec.fmtp});
        // Generic SDP emits raw feedback; WebRTC emits the codec's typed copy.
        for (const auto& item : codec.rtcpFeedback)
            attributes.push_back({"rtcp-fb", payload + " " + item.type +
                (item.parameter.empty() ? "" : " " + item.parameter)});
    }
    result.codecs = std::move(codecs);
    media = std::move(result);
}

std::string Sdp::Serialize(const SdpSession& session)
{
    if (session.profile == SdpProfile::WebRtc)
        return Serialize(detail::BuildWebRtcAttributes(session));
    std::ostringstream output;
    const auto value = [](const std::string& text, const char* fallback) {
        return text.empty() ? std::string(fallback) : text;
    };
    const auto attribute = [&](const SdpAttribute& item) {
        output << "a=" << item.key;
        if (!item.value.empty()) output << ':' << item.value;
        output << "\r\n";
    };
    const auto connection = [&](const SdpConnection& item) {
        output << "c=" << item.net_type << ' ' << item.addr_type << ' ' << item.address << "\r\n";
    };
    const auto& origin = session.origin;
    output << "v=" << session.version << "\r\n"
        << "o=" << value(origin.username, "-") << ' ' << value(origin.sess_id, "0")
        << ' ' << value(origin.sess_version, "0") << ' ' << value(origin.net_type, "IN")
        << ' ' << value(origin.addr_type, "IP4") << ' ' << value(origin.unicast_address, "0.0.0.0")
        << "\r\ns=" << value(session.session_name, "-") << "\r\n";
    if (!session.conn.address.empty()) connection(session.conn);
    else if (!session.connection.empty()) output << "c=" << session.connection << "\r\n";
    output << "t=" << value(session.timing, "0 0") << "\r\n";
    for (const auto& item : session.attributes) attribute(item);
    for (const auto& media : session.medias)
    {
        output << "m=" << media.media << ' ' << media.port;
        if (media.portCount != 1) output << '/' << media.portCount;
        output << ' ' << media.proto;
        for (const auto& format : media.fmts) output << ' ' << format;
        output << "\r\n";
        if (!media.conn.address.empty()) connection(media.conn);
        std::unordered_set<int> maps;
        std::unordered_set<int> formats;
        for (const auto& item : media.attributes)
        {
            attribute(item);
            const auto payload = item.value.substr(0, item.value.find_first_of(" \t"));
            int number = -1;
            const auto parsed = std::from_chars(payload.data(), payload.data() + payload.size(), number);
            if (parsed.ec == std::errc{} && parsed.ptr == payload.data() + payload.size())
            {
                if (item.key == "rtpmap") maps.insert(number);
                if (item.key == "fmtp") formats.insert(number);
            }
        }
        // Raw attributes are authoritative; support callers constructing only
        // the typed RTP maps without emitting duplicate attribute lines.
        for (const auto& map : media.rtpmaps)
        {
            const auto payload = std::to_string(map.payloadType);
            if (maps.insert(map.payloadType).second)
                attribute({"rtpmap", payload + " " + map.encodingName + "/" + std::to_string(map.clockRate)
                    + (map.channels > 0 ? "/" + std::to_string(map.channels) : "")});
        }
        for (const auto& format : media.fmtps)
        {
            const auto payload = std::to_string(format.payloadType);
            if (formats.insert(format.payloadType).second) attribute({"fmtp", payload + " " + format.params});
        }
    }
    return output.str();
}
}
