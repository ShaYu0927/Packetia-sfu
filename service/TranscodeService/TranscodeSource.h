#pragma once

#include "MediaFrameTypes.h"

#include <string>

namespace service::transcode {

// Domain identity/filter. Independent of frame routers and runtime sessions.
struct TranscodeSource
{
    uint64_t endpoint_id = 0;
    media::TrackId track_id = 0;
    std::string session_id;
    std::string stream_id;
    uint32_t ssrc = 0;
    // 非零时绑定内核发布实例；同名重发不会自动成为此任务的新输入。
    uint32_t media_stream_id = 0;

    bool Valid() const noexcept { return endpoint_id != 0; }
    bool SameTrack(const TranscodeSource& other) const noexcept
    {
        return endpoint_id == other.endpoint_id && track_id == other.track_id;
    }
    bool Matches(const TranscodeSource& actual) const noexcept
    {
        return SameTrack(actual) && (session_id.empty() || session_id == actual.session_id) &&
            (stream_id.empty() || stream_id == actual.stream_id) && (!ssrc || ssrc == actual.ssrc) &&
            (!media_stream_id || media_stream_id == actual.media_stream_id);
    }
};

struct TranscodeOutput
{
    uint64_t endpoint_id = 0;
    std::string session_id;
    std::string stream_id;

    bool Valid() const noexcept { return endpoint_id && !session_id.empty() && !stream_id.empty(); }
    std::string StreamFor(const std::string& rendition_id) const { return stream_id + "/" + rendition_id; }
};

} // namespace service::transcode
