#include "TranscodeTypes.h"

#include <unordered_set>

namespace service::transcode {

bool VideoTranscodeConfig::Validate(std::string& error) const
{
    error.clear();
    if (renditions.empty() || renditions.size() > 8 || frame_rate <= 0 || frame_rate > 240 ||
        keyframe_interval <= 0 || !media::codec::IsVideoCodec(input_codec) || !decoder.Valid() ||
        (timestamp_mode != InputTimestampMode::MediaTime &&
                                   timestamp_mode != InputTimestampMode::Rtp32)) {
        error = "invalid transcode configuration";
        return false;
    }
    std::unordered_set<std::string> ids;
    std::unordered_set<media::TrackId> tracks;
    for (const auto& out : renditions) {
        const auto& profile = out.encoding;
        if (!media::codec::ParametersMatch(out.codec, out.parameters) || !out.backend.Valid() ||
            out.id.empty() || !ids.insert(out.id).second || !out.track_id ||
            !tracks.insert(out.track_id).second || profile.width < 2 || profile.width > 4096 ||
            profile.height < 2 || profile.height > 4096 || (profile.width & 1) || (profile.height & 1) ||
            profile.bitrate_bps <= 0 || profile.bitrate_bps > 100000000) {
            error = "invalid or duplicate output rendition";
            return false;
        }
    }
    return true;
}

} // namespace service::transcode
