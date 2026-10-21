#pragma once

#include "MediaFrameTypes.h"
#include "VideoCodecConfig.h"

#include <cstdint>
#include <string>
#include <vector>

namespace service::transcode {

struct VideoEncodingProfile
{
    int width = 0;
    int height = 0;
    int bitrate_bps = 0;
};

struct VideoRendition
{
    std::string id;
    media::TrackId track_id = 0;
    VideoEncodingProfile encoding;
    media::CodecType codec = media::CodecType::H264;
    media::codec::BackendRequest backend;
    media::codec::VideoCodecParameters parameters;
};

enum class InputTimestampMode
{
    // Signed, already extended PTS in the frame's declared time base.
    MediaTime,
    // Raw 32-bit RTP PTS, in 1/90000 units, as produced by H264RtpDepacketizer.
    Rtp32,
};

struct VideoTranscodeConfig
{
    std::vector<VideoRendition> renditions{
        {"720p", 1, {1280, 720, 2000000}},
        {"360p", 2, {640, 360, 600000}},
    };
    InputTimestampMode timestamp_mode = InputTimestampMode::MediaTime;
    // Encoder timing/GOP hint; input timestamps and frame rate are preserved.
    int frame_rate = 30;
    int keyframe_interval = 60;
    media::CodecType input_codec = media::CodecType::H264;
    media::codec::BackendRequest decoder;
    bool Validate(std::string& error) const;
};

struct VideoTranscodeStats
{
    uint64_t input_access_units = 0;
    uint64_t decoded_frames = 0;
    uint64_t encoded_frames = 0; // Sum across all renditions.
};

} // namespace service::transcode
