#include "SdpTrackBinding.h"
#include "CodecNames.h"
#include "Sdp.h"
#include "logger.h"
#include <algorithm>
#include <sstream>
#include <utility>

namespace media
{
namespace
{
MediaType ParseMediaType(const std::string& type)
{
    if (type == "video") return MediaType::Video;
    if (type == "audio") return MediaType::Audio;
    return MediaType::Unknown;
}

bool ParsePayloadType(const std::string& fmt, int& pt)
{
    try
    {
        pt = std::stoi(fmt);
        return true;
    }
    catch (...)
    {
        return false;
    }
}

void FillRtpMap(const sdp::SdpMedia& media, MediaType type, uint8_t pt, RtpPayloadInfo& info)
{
    for (const auto& codec : media.codecs)
    {
        if (codec.payloadType != pt) continue;
        info.codec_name = codec.encodingName;
        info.codec = CodecTypeFromName(codec.encodingName);
        info.clock_rate = codec.clockRate;
        info.channels = type == MediaType::Audio ? std::max(1, codec.channels) : 0;
        return;
    }
    for (const auto& rtpmap : media.rtpmaps)
    {
        if (rtpmap.payloadType != static_cast<int>(pt))
        {
            continue;
        }

        info.codec_name = rtpmap.encodingName;
        info.codec = CodecTypeFromName(rtpmap.encodingName);
        info.clock_rate = rtpmap.clockRate;
        info.channels = rtpmap.channels;
        if (type == MediaType::Audio && info.channels <= 0)
        {
            info.channels = 1;
        }

        if (type == MediaType::Video && info.clock_rate <= 0)
        {
            info.clock_rate = 90000;
        }

        return;
    }

    if (type == MediaType::Audio && pt == 0)
    {
        info.codec_name = "PCMU";
        info.codec = CodecType::PCMU;
        info.clock_rate = 8000;
        info.channels = 1;
        return;
    }

    if (type == MediaType::Audio && pt == 8)
    {
        info.codec_name = "PCMA";
        info.codec = CodecType::PCMA;
        info.clock_rate = 8000;
        info.channels = 1;
        return;
    }

    LOG_INFO("[SDP] rtpmap not found", " media=", media.media, " pt=", static_cast<int>(pt));
}
void FillFmtp(const sdp::SdpMedia& media, uint8_t pt, RtpPayloadInfo& info)
{
    for (const auto& codec : media.codecs)
    {
        if (codec.payloadType != pt) continue;
        info.fmtp = codec.fmtp;
        return;
    }
    for (const auto& fmtp : media.fmtps)
    {
        if (fmtp.payloadType != static_cast<int>(pt))
        {
            continue;
        }

        info.fmtp = fmtp.params;
        return;
    }

    LOG_INFO("[SDP] fmtp not found", " media=", media.media, " pt=", static_cast<int>(pt));
}

} // namespace

uint8_t FindRtpExtensionId(const sdp::SdpMedia& media, const std::string& uri)
{
    for (const auto& ext : media.headerExtensions)
        if (ext.uri == uri && ext.id > 0 && ext.id < 256) return static_cast<uint8_t>(ext.id);
    for (const auto& attr : media.attributes)
    {
        if (attr.key != "extmap") continue;
        std::istringstream input(attr.value);
        std::string id, name;
        if (!(input >> id >> name) || name != uri) continue;
        try
        {
            size_t end = 0;
            const int value = std::stoi(id, &end);
            if (value > 0 && value < 256 && (end == id.size() || id[end] == '/'))
                return static_cast<uint8_t>(value);
        }
        catch (...) {}
    }
    return 0;
}

RtpTrackContext RtpStreamContextBuilder::BuildTrackFromSdp(const sdp::SdpMedia& media, int index)
{
    RtpTrackContext track;
    track.media_index = index;
    track.media_type = ParseMediaType(media.media);
    const auto twcc = FindRtpExtensionId(media,
        "http://www.ietf.org/id/draft-holmer-rmcat-transport-wide-cc-extensions-01");
    if (twcc <= 14) track.transport_cc_extension_id = twcc;

    auto formats = media.fmts;
    if (!media.codecs.empty())
    {
        formats.clear();
        for (const auto& codec : media.codecs) formats.push_back(std::to_string(codec.payloadType));
    }
    for (const auto& fmt : formats)
    {
        int pt = 0;
        if (!ParsePayloadType(fmt, pt) || pt < 0 || pt > 127)
        {
            LOG_ERROR("[SDP] invalid payload type fmt=", fmt, " media_index=", index);
            continue;
        }
        RtpPayloadInfo payload;
        payload.payload_type = static_cast<uint8_t>(pt);
        FillRtpMap(media, track.media_type, payload.payload_type, payload);
        FillFmtp(media, payload.payload_type, payload);
        track.payloads.push_back(std::move(payload));
    }
    return track;
}

std::shared_ptr<const RtpStreamContext> RtpStreamContextBuilder::BuildFromSdp(const sdp::SdpSession& session)
{
    auto context = std::make_shared<RtpStreamContext>();
    context->tracks.reserve(session.medias.size());
    for (size_t i = 0; i < session.medias.size(); ++i)
        context->tracks.push_back(BuildTrackFromSdp(session.medias[i], static_cast<int>(i)));
    return context;
}
} // namespace media
