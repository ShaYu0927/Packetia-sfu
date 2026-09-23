#include "SdpWebRtc.h"

#include <algorithm>
#include <charconv>
#include <cctype>
#include <limits>
#include <map>
#include <set>
#include <sstream>
#include <stdexcept>

namespace sdp::detail
{
namespace
{
struct ParseError : std::runtime_error { using std::runtime_error::runtime_error; };

void Require(bool condition, const std::string& message)
{
    if (!condition) throw ParseError(message);
}

std::vector<std::string> Words(const std::string& text)
{
    std::vector<std::string> result;
    std::istringstream input(text);
    for (std::string token; input >> token;) result.push_back(std::move(token));
    return result;
}

std::string Lower(std::string text)
{
    for (auto& ch : text) ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
    return text;
}

uint64_t Number(const std::string& text, uint64_t maximum, const std::string& field)
{
    uint64_t value = 0;
    Require(!text.empty() && text.front() >= '0' && text.front() <= '9', "Invalid integer in " + field);
    const auto result = std::from_chars(text.data(), text.data() + text.size(), value);
    Require(result.ec == std::errc{} && result.ptr == text.data() + text.size() && value <= maximum,
        "Invalid integer in " + field + ": " + text);
    return value;
}

void Token(const std::string& text, const std::string& field)
{
    Require(!text.empty() && std::all_of(text.begin(), text.end(), [](unsigned char ch) {
        return ch > 32 && ch < 127;
    }), "Invalid token in " + field);
}

void Once(std::set<std::string>& seen, const std::string& key)
{
    Require(seen.insert(key).second, "Duplicate SDP attribute: " + key);
}

void Flag(const sdp::SdpAttribute& attribute)
{
    Require(attribute.value.empty(), "Unexpected value on a=" + attribute.key);
}

MediaDirection Direction(const std::string& text)
{
    if (text == "sendrecv") return MediaDirection::SendRecv;
    if (text == "sendonly") return MediaDirection::SendOnly;
    if (text == "recvonly") return MediaDirection::RecvOnly;
    if (text == "inactive") return MediaDirection::Inactive;
    throw ParseError("Invalid media direction: " + text);
}

bool IsDirection(const std::string& text)
{
    return text == "sendrecv" || text == "sendonly" || text == "recvonly" || text == "inactive";
}

void ValidateCandidate(const std::string& value)
{
    const auto fields = Words(value);
    Require(fields.size() >= 8 && fields[6] == "typ" && (fields.size() - 8) % 2 == 0,
        "Invalid ICE candidate fields");
    Require(Number(fields[1], 256, "candidate component") > 0, "Invalid ICE candidate component");
    Number(fields[3], UINT32_MAX, "candidate priority");
    Require(Number(fields[5], 65535, "candidate port") > 0, "Invalid ICE candidate port");
    for (std::size_t i = 8; i < fields.size(); i += 2)
    {
        if (fields[i] == "rport") Number(fields[i + 1], 65535, "candidate rport");
        else if (fields[i] == "generation" || fields[i] == "network-id" || fields[i] == "network-cost")
            Number(fields[i + 1], UINT32_MAX, "candidate " + fields[i]);
    }
}

DtlsFingerprint Fingerprint(const std::string& value)
{
    const auto fields = Words(value);
    Require(fields.size() == 2, "Invalid DTLS fingerprint fields");
    const auto algorithm = Lower(fields[0]);
    const std::map<std::string, std::size_t> sizes = {
        {"sha-1", 20}, {"sha-224", 28}, {"sha-256", 32}, {"sha-384", 48}, {"sha-512", 64}};
    const auto size = sizes.find(algorithm);
    Require(size != sizes.end(), "Unsupported DTLS fingerprint algorithm");
    auto digest = fields[1];
    Require(digest.size() == size->second * 3 - 1, "Invalid DTLS fingerprint length");
    for (std::size_t i = 0; i < digest.size(); ++i)
        Require(i % 3 == 2 ? digest[i] == ':' : std::isxdigit(static_cast<unsigned char>(digest[i])) != 0,
            "Invalid DTLS fingerprint bytes");
    for (auto& ch : digest) ch = static_cast<char>(std::toupper(static_cast<unsigned char>(ch)));
    return {algorithm, std::move(digest)};
}

RtpHeaderExtensionParameters Extension(const std::string& value)
{
    const auto fields = Words(value);
    Require(fields.size() >= 2, "Invalid extmap fields");
    const auto slash = fields[0].find('/');
    RtpHeaderExtensionParameters result;
    result.id = static_cast<int>(Number(fields[0].substr(0, slash), 255, "extmap id"));
    Require(result.id != 0, "Invalid extmap id");
    if (slash != std::string::npos) result.direction = Direction(fields[0].substr(slash + 1));
    result.uri = fields[1];
    Require(result.uri != "urn:ietf:params:rtp-hdrext:encrypt", "Encrypted extmap is not supported");
    for (std::size_t i = 2; i < fields.size(); ++i)
    {
        if (!result.attributes.empty()) result.attributes += ' ';
        result.attributes += fields[i];
    }
    return result;
}

void CommonAttributes(const std::vector<sdp::SdpAttribute>& attributes,
    IceParameters& ice, DtlsParameters& dtls, MediaDirection& direction,
    std::vector<RtpHeaderExtensionParameters>& extensions)
{
    std::set<std::string> seen;
    std::set<std::string> fingerprint_algorithms;
    std::set<int> extension_ids;
    bool have_candidates = false;
    bool have_fingerprints = false;
    bool have_extensions = false;
    for (const auto& attribute : attributes)
    {
        const auto& key = attribute.key;
        const auto& value = attribute.value;
        if (key == "ice-ufrag" || key == "ice-pwd")
        {
            Once(seen, key);
            Token(value, key);
            (key == "ice-ufrag" ? ice.ufrag : ice.pwd) = value;
        }
        else if (key == "ice-options")
        {
            Once(seen, key);
            ice.options = Words(value);
            Require(!ice.options.empty(), "Empty ice-options");
            std::set<std::string> options;
            for (const auto& option : ice.options)
                Require(options.insert(option).second, "Duplicate ICE option");
        }
        else if (key == "ice-lite" || key == "end-of-candidates")
        {
            Once(seen, key);
            Flag(attribute);
            if (key == "ice-lite") ice.iceLite = true;
            else ice.endOfCandidates = true;
        }
        else if (key == "candidate")
        {
            ValidateCandidate(value);
            if (!have_candidates) { ice.candidates.clear(); have_candidates = true; }
            Require(std::find(ice.candidates.begin(), ice.candidates.end(), value) == ice.candidates.end(),
                "Duplicate ICE candidate");
            ice.candidates.push_back(value);
        }
        else if (key == "setup")
        {
            Once(seen, key);
            if (value == "actpass") dtls.setup = DtlsSetup::ActPass;
            else if (value == "active") dtls.setup = DtlsSetup::Active;
            else if (value == "passive") dtls.setup = DtlsSetup::Passive;
            else if (value == "holdconn") dtls.setup = DtlsSetup::HoldConn;
            else throw ParseError("Invalid DTLS setup role");
        }
        else if (key == "fingerprint")
        {
            auto fingerprint = Fingerprint(value);
            Require(fingerprint_algorithms.insert(fingerprint.algorithm).second,
                "Duplicate DTLS fingerprint algorithm");
            if (!have_fingerprints) { dtls.fingerprints.clear(); have_fingerprints = true; }
            dtls.fingerprints.push_back(std::move(fingerprint));
        }
        else if (IsDirection(key))
        {
            Once(seen, "direction");
            Flag(attribute);
            direction = Direction(key);
        }
        else if (key == "extmap")
        {
            auto extension = Extension(value);
            Require(extension_ids.insert(extension.id).second, "Duplicate extmap id");
            if (!have_extensions) { extensions.clear(); have_extensions = true; }
            extensions.push_back(std::move(extension));
        }
        else if (key == "extmap-allow-mixed") { Once(seen, key); Flag(attribute); }
    }
}

RtpCodecParameters StaticCodec(const std::string& media, int payload)
{
    RtpCodecParameters result;
    result.payloadType = payload;
    result.channels = 1;
    if (media == "audio")
    {
        if (payload == 0) { result.encodingName = "PCMU"; result.clockRate = 8000; }
        if (payload == 3) { result.encodingName = "GSM"; result.clockRate = 8000; }
        if (payload == 8) { result.encodingName = "PCMA"; result.clockRate = 8000; }
        if (payload == 9) { result.encodingName = "G722"; result.clockRate = 8000; }
        if (payload == 13) { result.encodingName = "CN"; result.clockRate = 8000; }
        if (payload == 18) { result.encodingName = "G729"; result.clockRate = 8000; }
    }
    return result;
}

bool IsRtp(const sdp::SdpMedia& media)
{
    return media.proto.find("RTP/") != std::string::npos;
}

void CodecAttributes(SdpMedia& media)
{
    if (!IsRtp(media)) return;
    std::map<int, sdp::SdpRtpMap> maps;
    std::map<int, std::string> formats;
    std::set<int> payloads;
    for (const auto& value : media.fmts)
    {
        const auto pt = static_cast<int>(Number(value, 127, "m= payload type"));
        Require(payloads.insert(pt).second, "Duplicate payload type in m=");
    }
    for (const auto& map : media.rtpmaps)
    {
        Require(payloads.count(map.payloadType) != 0, "rtpmap payload is absent from m=");
        Require(maps.emplace(map.payloadType, map).second, "Duplicate rtpmap payload");
    }
    for (const auto& format : media.fmtps)
    {
        Require(payloads.count(format.payloadType) != 0, "fmtp payload is absent from m=");
        Require(formats.emplace(format.payloadType, format.params).second, "Duplicate fmtp payload");
    }
    std::map<int, std::vector<RtcpFeedback>> feedback;
    for (const auto& attribute : media.attributes)
    {
        if (attribute.key != "rtcp-fb") continue;
        const auto fields = Words(attribute.value);
        Require(fields.size() >= 2, "Invalid rtcp-fb fields");
        RtcpFeedback item{fields[1]};
        for (std::size_t i = 2; i < fields.size(); ++i)
        {
            if (!item.parameter.empty()) item.parameter += ' ';
            item.parameter += fields[i];
        }
        std::vector<RtcpFeedback>* target = &media.rtcpFeedback;
        if (fields[0] != "*")
        {
            const auto pt = static_cast<int>(Number(fields[0], 127, "rtcp-fb payload"));
            Require(payloads.count(pt) != 0, "rtcp-fb payload is absent from m=");
            target = &feedback[pt];
        }
        Require(std::none_of(target->begin(), target->end(), [&](const auto& previous) {
            return previous.type == item.type && previous.parameter == item.parameter;
        }), "Duplicate rtcp-fb value");
        target->push_back(std::move(item));
    }
    for (const auto& value : media.fmts)
    {
        const auto pt = static_cast<int>(Number(value, 127, "m= payload type"));
        auto codec = StaticCodec(media.media, pt);
        const auto found = maps.find(pt);
        if (found != maps.end())
        {
            codec.encodingName = found->second.encodingName;
            codec.clockRate = found->second.clockRate;
            codec.channels = found->second.channels;
        }
        Require(!codec.encodingName.empty() || (media.port == 0 && !media.bundleOnly),
            "Missing rtpmap for active payload " + value);
        codec.fmtp = formats[pt];
        codec.rtcpFeedback = std::move(feedback[pt]);
        media.codecs.push_back(std::move(codec));
    }
}

void MediaAttributes(SdpMedia& media)
{
    std::set<std::string> seen;
    for (const auto& attribute : media.attributes)
    {
        const auto& key = attribute.key;
        const auto& value = attribute.value;
        if (key == "mid") { Once(seen, key); Token(value, key); media.mid = value; }
        else if (key == "rtcp-mux" || key == "rtcp-rsize" || key == "bundle-only")
        {
            Once(seen, key);
            Flag(attribute);
            if (key == "rtcp-mux") media.rtcpMux = true;
            else if (key == "rtcp-rsize") media.rtcpRsize = true;
            else media.bundleOnly = true;
        }
        else if (key == "msid")
        {
            const auto fields = Words(value);
            Require(!fields.empty() && fields.size() <= 2, "Invalid msid");
            Require(std::find(media.msids.begin(), media.msids.end(), value) == media.msids.end(), "Duplicate msid");
            media.msids.push_back(value);
        }
        else if (key == "ssrc")
        {
            const auto space = value.find_first_of(" \t");
            Require(space != std::string::npos, "Missing SSRC attribute");
            const auto id = static_cast<uint32_t>(Number(value.substr(0, space), UINT32_MAX, "ssrc"));
            const auto start = value.find_first_not_of(" \t", space);
            Require(start != std::string::npos, "Missing SSRC attribute");
            const auto colon = value.find(':', start);
            sdp::SdpAttribute item{value.substr(start, colon == std::string::npos ? colon : colon - start),
                colon == std::string::npos ? "" : value.substr(colon + 1)};
            Token(item.key, "ssrc attribute");
            auto found = std::find_if(media.ssrcs.begin(), media.ssrcs.end(),
                [&](const auto& ssrc) { return ssrc.ssrc == id; });
            if (found == media.ssrcs.end())
            {
                media.ssrcs.push_back({id, {}});
                found = std::prev(media.ssrcs.end());
            }
            if (item.key == "cname")
                Require(std::none_of(found->attributes.begin(), found->attributes.end(), [](const auto& previous) {
                    return previous.key == "cname";
                }), "Duplicate SSRC cname");
            found->attributes.push_back(std::move(item));
        }
        else if (key == "ssrc-group")
        {
            const auto fields = Words(value);
            Require(fields.size() >= 2, "Invalid SSRC group");
            RtpSsrcGroup group;
            group.semantics = fields[0];
            std::set<uint32_t> members;
            for (std::size_t i = 1; i < fields.size(); ++i)
            {
                const auto id = static_cast<uint32_t>(Number(fields[i], UINT32_MAX, "ssrc-group"));
                Require(members.insert(id).second, "Duplicate SSRC in group");
                group.ssrcs.push_back(id);
            }
            media.ssrcGroups.push_back(std::move(group));
        }
        else if (key == "rtcp")
        {
            Once(seen, key);
            const auto fields = Words(value);
            Require(fields.size() == 1 || fields.size() == 4, "Invalid rtcp address");
            Number(fields[0], 65535, "rtcp port");
        }
        else if (key == "sctp-port" || key == "max-message-size")
        {
            Once(seen, key);
            Number(value, key == "sctp-port" ? 65535 : UINT64_MAX, key);
        }
        else if (key == "group") throw ParseError("group must be session-level");
    }
    Require(!media.mid.empty(), "Missing media MID");
    CodecAttributes(media);
}
void RemoveModeledAttributes(std::vector<SdpAttribute>& attributes)
{
    attributes.erase(std::remove_if(attributes.begin(), attributes.end(), [](const auto& attribute) {
        const auto& key = attribute.key;
        if (key == "group")
        {
            const auto fields = Words(attribute.value);
            return !fields.empty() && fields.front() == "BUNDLE";
        }
        return IsDirection(key) || key == "ice-ufrag" || key == "ice-pwd" ||
            key == "ice-options" || key == "ice-lite" || key == "candidate" ||
            key == "end-of-candidates" || key == "setup" || key == "fingerprint" ||
            key == "extmap" || key == "mid" || key == "rtcp-mux" ||
            key == "rtcp-rsize" || key == "bundle-only" || key == "rtpmap" ||
            key == "fmtp" || key == "rtcp-fb" || key == "msid" ||
            key == "ssrc" || key == "ssrc-group";
    }), attributes.end());
}

const char* DirectionName(MediaDirection direction)
{
    switch (direction)
    {
    case MediaDirection::SendRecv: return "sendrecv";
    case MediaDirection::SendOnly: return "sendonly";
    case MediaDirection::RecvOnly: return "recvonly";
    default: return "inactive";
    }
}

void AddTransport(std::vector<SdpAttribute>& attributes, const IceParameters& ice,
                  const DtlsParameters& dtls, bool sessionLevel = false)
{
    if (!ice.ufrag.empty()) attributes.push_back({"ice-ufrag", ice.ufrag});
    if (!ice.pwd.empty()) attributes.push_back({"ice-pwd", ice.pwd});
    if (!ice.options.empty())
    {
        std::string value;
        for (const auto& option : ice.options)
        {
            if (!value.empty()) value += ' ';
            value += option;
        }
        attributes.push_back({"ice-options", std::move(value)});
    }
    if (!sessionLevel)
    {
        for (const auto& candidate : ice.candidates) attributes.push_back({"candidate", candidate});
        if (ice.endOfCandidates) attributes.push_back({"end-of-candidates", ""});
    }
    const char* setup = nullptr;
    switch (dtls.setup)
    {
    case DtlsSetup::ActPass: setup = "actpass"; break;
    case DtlsSetup::Active: setup = "active"; break;
    case DtlsSetup::Passive: setup = "passive"; break;
    case DtlsSetup::HoldConn: setup = "holdconn"; break;
    default: break;
    }
    if (setup) attributes.push_back({"setup", setup});
    for (const auto& fp : dtls.fingerprints) attributes.push_back({"fingerprint", fp.algorithm + " " + fp.value});
}

void AddFeedback(std::vector<SdpAttribute>& attributes, const std::string& pt,
                 const std::vector<RtcpFeedback>& feedback)
{
    for (const auto& item : feedback)
        attributes.push_back({"rtcp-fb", pt + " " + item.type +
            (item.parameter.empty() ? "" : " " + item.parameter)});
}
} // namespace

bool ParseWebRtcAttributes(const std::string& text, SdpSession& result, std::string& error)
{
    try
    {
        std::istringstream lines(text);
        std::string first;
        while (std::getline(lines, first))
        {
            if (!first.empty() && first.back() == '\r') first.pop_back();
            if (!first.empty()) break;
        }
        Require(first == "v=0" && !result.origin.sess_id.empty()
            && !result.session_name.empty() && !result.timing.empty(),
            "WebRTC SDP requires v=, o=, s= and t= session fields");
        Require(result.origin.net_type == "IN"
            && (result.origin.addr_type == "IP4" || result.origin.addr_type == "IP6"),
            "Invalid WebRTC origin network/address type");
        Require(!result.medias.empty(), "WebRTC SDP contains no media");
        MediaDirection direction = MediaDirection::SendRecv;
        std::vector<RtpHeaderExtensionParameters> extensions;
        CommonAttributes(result.attributes, result.ice, result.dtls, direction, extensions);
        bool bundle_seen = false;
        for (const auto& attribute : result.attributes)
        {
            if (attribute.key == "group")
            {
                const auto fields = Words(attribute.value);
                Require(!fields.empty(), "Empty SDP group");
                if (fields[0] != "BUNDLE") continue;
                Require(!bundle_seen && fields.size() > 1, "Duplicate or empty BUNDLE group");
                bundle_seen = true;
                std::set<std::string> members;
                for (std::size_t i = 1; i < fields.size(); ++i)
                {
                    Require(members.insert(fields[i]).second, "Duplicate MID in BUNDLE group");
                    result.bundle.mids.push_back(fields[i]);
                }
            }
            else if (attribute.key == "mid" || attribute.key == "rtpmap" || attribute.key == "fmtp"
                || attribute.key == "rtcp-fb" || attribute.key == "rtcp-mux" || attribute.key == "rtcp-rsize"
                || attribute.key == "bundle-only" || attribute.key == "ssrc" || attribute.key == "ssrc-group")
                throw ParseError(attribute.key + " must be media-level");
        }
        std::set<std::string> mids;
        for (auto& media : result.medias)
        {
            Require(media.portCount == 1, "Multiple media ports are not supported by WebRTC");
            media.direction = direction;
            media.ice = result.ice;
            media.dtls = result.dtls;
            media.headerExtensions = extensions;
            CommonAttributes(media.attributes, media.ice, media.dtls, media.direction, media.headerExtensions);
            MediaAttributes(media);
            Require(mids.insert(media.mid).second, "Duplicate media MID: " + media.mid);
        }
        for (const auto& mid : result.bundle.mids)
            Require(mids.count(mid) != 0, "BUNDLE references an unknown MID: " + mid);
        for (const auto& media : result.medias)
            if (media.bundleOnly)
                Require(std::find(result.bundle.mids.begin(), result.bundle.mids.end(), media.mid) != result.bundle.mids.end(),
                    "bundle-only media is absent from BUNDLE group");
        // Known attributes have one writable representation. Media transport
        // parameters are normalized to effective values during parsing.
        RemoveModeledAttributes(result.attributes);
        for (auto& media : result.medias)
        {
            RemoveModeledAttributes(media.attributes);
            media.rtpmaps.clear();
            media.fmtps.clear();
        }
        error.clear();
        return true;
    }
    catch (const ParseError& failure)
    {
        error = failure.what();
        return false;
    }
}

SdpSession BuildWebRtcAttributes(const SdpSession& session)
{
    auto raw = session;
    raw.profile = SdpProfile::Generic;
    RemoveModeledAttributes(raw.attributes);
    if (session.ice.iceLite) raw.attributes.push_back({"ice-lite", ""});
    AddTransport(raw.attributes, session.ice, session.dtls, true);
    if (!session.bundle.mids.empty())
    {
        std::string group = "BUNDLE";
        for (const auto& mid : session.bundle.mids) group += " " + mid;
        raw.attributes.push_back({"group", std::move(group)});
    }
    for (auto& media : raw.medias)
    {
        RemoveModeledAttributes(media.attributes);
        media.rtpmaps.clear();
        media.fmtps.clear();
        auto& attributes = media.attributes;
        if (!media.mid.empty()) attributes.push_back({"mid", media.mid});
        attributes.push_back({DirectionName(media.direction), ""});
        if (media.rtcpMux) attributes.push_back({"rtcp-mux", ""});
        if (media.rtcpRsize) attributes.push_back({"rtcp-rsize", ""});
        if (media.bundleOnly) attributes.push_back({"bundle-only", ""});
        AddTransport(attributes, media.ice, media.dtls);
        // Rejected m-lines retain their format tokens even without codec maps.
        if (IsRtp(media) && (media.port != 0 || media.bundleOnly) && !media.codecs.empty())
        {
            media.fmts.clear();
            for (const auto& codec : media.codecs) media.fmts.push_back(std::to_string(codec.payloadType));
        }
        for (const auto& codec : media.codecs)
        {
            if (codec.encodingName.empty()) continue;
            const auto pt = std::to_string(codec.payloadType);
            attributes.push_back({"rtpmap", pt + " " + codec.encodingName + "/" +
                std::to_string(codec.clockRate) + (codec.channels > 1 ? "/" + std::to_string(codec.channels) : "")});
            if (!codec.fmtp.empty()) attributes.push_back({"fmtp", pt + " " + codec.fmtp});
            AddFeedback(attributes, pt, codec.rtcpFeedback);
        }
        AddFeedback(attributes, "*", media.rtcpFeedback);
        for (const auto& extension : media.headerExtensions)
            attributes.push_back({"extmap", std::to_string(extension.id) + "/" +
                DirectionName(extension.direction) + " " + extension.uri +
                (extension.attributes.empty() ? "" : " " + extension.attributes)});
        for (const auto& msid : media.msids) attributes.push_back({"msid", msid});
        for (const auto& source : media.ssrcs)
            for (const auto& attr : source.attributes)
                attributes.push_back({"ssrc", std::to_string(source.ssrc) + " " + attr.key +
                    (attr.value.empty() ? "" : ":" + attr.value)});
        for (const auto& group : media.ssrcGroups)
        {
            std::string value = group.semantics;
            for (auto ssrc : group.ssrcs) value += " " + std::to_string(ssrc);
            attributes.push_back({"ssrc-group", std::move(value)});
        }
    }
    return raw;
}
} // namespace sdp::detail
