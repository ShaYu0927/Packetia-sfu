#ifndef PACKETIA_MEDIA_SDP_TRACK_BINDING_H
#define PACKETIA_MEDIA_SDP_TRACK_BINDING_H

#include "RtpTypes.h"
#include "StreamContext.h"
#include <sstream>

namespace media
{
// Convert a negotiated media section into parameters understood by the RTP
// pipeline. Callers select active sections and supply stable track identities.
inline uint8_t FindRtpExtensionId(const sdp::SdpMedia& media, const std::string& uri)
{
    for (const auto& ext : media.headerExtensions)
        if (ext.uri == uri && ext.id > 0 && ext.id < 256) return static_cast<uint8_t>(ext.id);
    for (const auto& attr : media.attributes) {
        if (attr.key != "extmap") continue;
        std::istringstream input(attr.value);
        std::string id, name;
        if (!(input >> id >> name) || name != uri) continue;
        try {
            size_t end = 0;
            const int value = std::stoi(id, &end);
            if (value > 0 && value < 256 && (end == id.size() || id[end] == '/'))
                return static_cast<uint8_t>(value);
        } catch (...) {}
    }
    return 0;
}

inline bool BuildRtpTrackInfo(const sdp::SdpMedia& media, int index, ::TrackInfo& result,
                              std::string* error = nullptr)
{
    sdp::SdpSession session;
    session.medias.push_back(media);
    const auto context = StreamContextBuilder::BuildFromSdp(session, {}, {});
    for (const auto& payload : context->tracks.front().payloads) {
        const auto codec = StringToCodecId(payload.codec_name);
        const bool audio = media.media == "audio";
        const bool supported = audio
            ? codec == CodecId::OPUS || codec == CodecId::PCMU || codec == CodecId::PCMA || codec == CodecId::AAC
            : media.media == "video" && codec == CodecId::H264;
        if (!supported || payload.sample_rate <= 0) continue;
        ::TrackInfo info;
        info.track_index = index;
        info.type = audio ? TrackAudio : TrackVideo;
        info.codec_id = codec;
        info.codec_name = payload.codec_name;
        info.payload_type = payload.payload_type;
        info.clock_rate = payload.sample_rate;
        info.channels = payload.channels;
        info.fmtp = payload.fmtp;
        info.control = media.GetAttribute("control");
        const auto twcc = FindRtpExtensionId(media,
            "http://www.ietf.org/id/draft-holmer-rmcat-transport-wide-cc-extensions-01");
        if (twcc <= 14) info.transport_cc_extension_id = twcc;
        result = std::move(info);
        if (error) error->clear();
        return true;
    }
    if (error) *error = "no supported RTP codec in media track " + std::to_string(index);
    return false;
}
}
#endif
