#include "RtpTrackBinding.h"

namespace media
{
namespace
{
bool Supported(const RtpTrackContext& track, const RtpPayloadInfo& payload)
{
    if (payload.payload_type > 127 || payload.clock_rate <= 0) return false;
    if (track.media_type == MediaType::Video) return payload.codec == CodecType::H264;
    return track.media_type == MediaType::Audio && payload.channels > 0 &&
        (payload.codec == CodecType::Opus || payload.codec == CodecType::PCMU ||
         payload.codec == CodecType::PCMA || payload.codec == CodecType::AAC);
}
}

bool SelectRtpPayload(const RtpTrackContext& track, std::size_t& selected, std::string* error)
{
    for (std::size_t i = 0; i < track.payloads.size(); ++i)
    {
        if (!Supported(track, track.payloads[i])) continue;
        selected = i;
        if (error) error->clear();
        return true;
    }
    if (error) *error = "no supported RTP codec in media track " + std::to_string(track.media_index);
    return false;
}

bool DescribeMediaTrack(const RtpTrackContext& track, std::size_t selected, MediaStreamTrack& result, std::string* error)
{
    if (track.media_index < 0 || selected >= track.payloads.size() || !Supported(track, track.payloads[selected]))
    {
        if (error) *error = "invalid RTP track selection";
        return false;
    }
    const auto& payload = track.payloads[selected];
    MediaStreamTrack description;
    description.id = static_cast<TrackId>(track.media_index);
    description.type = track.media_type;
    description.codec = payload.codec;
    if (track.media_type == MediaType::Audio)
    {
        description.sample_rate = static_cast<uint32_t>(payload.clock_rate);
        description.channels = payload.channels;
    }
    result = description;
    if (error) error->clear();
    return true;
}
}
