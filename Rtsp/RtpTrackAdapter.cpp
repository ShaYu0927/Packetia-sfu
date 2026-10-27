#include "RtpTrackAdapter.h"
#include "SdpTrackBinding.h"
#include "Sdp.h"
#include <utility>

namespace media
{
bool ToLegacyRtpTrackInfo(const RtpTrackContext& track, std::size_t selected,
                          ::TrackInfo& result, std::string* error)
{
    MediaStreamTrack description;
    if (!DescribeMediaTrack(track, selected, description, error)) return false;
    const auto& payload = track.payloads[selected];
    ::TrackInfo info;
    info.track_index = track.media_index;
    info.type = description.type == MediaType::Audio ? TrackAudio : TrackVideo;
    info.codec_id = ToLegacyCodecId(payload.codec);
    if (info.codec_id == CodecId::Unknown)
    {
        if (error) *error = "codec is unavailable in the legacy RTP receiver";
        return false;
    }
    info.codec_name = payload.codec_name;
    info.payload_type = payload.payload_type;
    info.clock_rate = static_cast<uint32_t>(payload.clock_rate);
    info.channels = payload.channels;
    info.fmtp = payload.fmtp;
    info.transport_cc_extension_id = track.transport_cc_extension_id;
    result = std::move(info);
    return true;
}

bool BuildRtpTrackInfo(const sdp::SdpMedia& media, int index, ::TrackInfo& result, std::string* error)
{
    const auto track = RtpStreamContextBuilder::BuildTrackFromSdp(media, index);
    std::size_t selected = 0;
    ::TrackInfo info;
    if (!SelectRtpPayload(track, selected, error) || !ToLegacyRtpTrackInfo(track, selected, info, error)) return false;
    info.control = media.GetAttribute("control");
    result = std::move(info);
    return true;
}
}
