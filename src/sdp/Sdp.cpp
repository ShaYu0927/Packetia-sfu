#include "Sdp.h"
#include "SdpUtil.h"
#include "SdpWebRtc.h"

#include <charconv>
#include <sstream>
#include <unordered_set>

namespace sdp 
{
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
