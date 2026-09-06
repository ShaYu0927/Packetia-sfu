#ifndef _MEDIA_FRAME_H_
#define _MEDIA_FRAME_H_

#include <cstddef>
#include <cstdint>
#include <memory>
#include <vector>

namespace media
{

using TrackId = uint32_t;

enum class MediaType
{
    Unknown = 0,
    Audio,
    Video
};

enum class CodecType
{
    Unknown = 0,

    H264,
    H265,

    Opus,
    PCMU,
    PCMA,
    AAC
};

enum class FrameIntegrity
{
    Complete = 0,
    Incomplete,
    Corrupted
};

struct FrameTimestamp
{
    int64_t dts = 0;
    int64_t pts = 0;

    int32_t time_base_num = 1;
    int32_t time_base_den = 1000;

    // Local arrival time retained for latency measurement and fallback.
    int64_t receive_time_ms = 0;
    // Sender wall-clock time derived from an RTCP SR RTP/NTP mapping.
    int64_t capture_time_ms = 0;
    bool capture_time_valid = false;

    bool Valid() const noexcept
    {
        return time_base_num > 0 && time_base_den > 0;
    }
};

struct RtpFrameInfo
{
    uint32_t ssrc = 0;

    uint32_t rtp_timestamp = 0;

    uint16_t first_sequence = 0;
    uint16_t last_sequence = 0;
    uint32_t packet_count = 0;
};

struct VideoFrameInfo
{
    bool is_idr = false;
    bool has_sps = false;
    bool has_pps = false;
    bool parameter_sets_injected = false;
    int32_t width = -1;
    int32_t height = -1;
};

struct MediaFrameInfo
{
    TrackId track_id = 0;

    MediaType media_type = MediaType::Unknown;

    CodecType codec = CodecType::Unknown;

    FrameTimestamp timestamp;

    FrameIntegrity integrity = FrameIntegrity::Complete;
};


enum class EncodedFrameType
{
    Unknown = 0,
    Key,
    Delta,
    Config,
    Audio
};

struct EncodedFrame
{
    using Ptr = std::shared_ptr<EncodedFrame>;
    using ConstPtr = std::shared_ptr<const EncodedFrame>;

    MediaFrameInfo info;

    RtpFrameInfo rtp;
    VideoFrameInfo video;

    EncodedFrameType frame_type     = EncodedFrameType::Unknown;
    uint32_t sample_count           = 0;

    uint32_t sample_rate            = 0;
    uint16_t channels               = 0;

    // Codec initialization bytes (AAC AudioSpecificConfig from SDP).
    std::shared_ptr<const std::vector<uint8_t>> codec_config;

    std::shared_ptr<const std::vector<uint8_t>> buffer;

    size_t offset                   = 0;
    size_t size                     = 0;

    const uint8_t* Data() const noexcept
    {
        if (!buffer || offset > buffer->size() || size > buffer->size() - offset)
        {
            return nullptr;
        }

        return buffer->data() + offset;
    }

    bool IsKeyFrame() const noexcept
    {
        return frame_type == EncodedFrameType::Key;
    }

    bool IsConfigFrame() const noexcept
    {
        return frame_type == EncodedFrameType::Config;
    }

    bool IsComplete() const noexcept
    {
        return info.integrity == FrameIntegrity::Complete;
    }

    bool Valid() const noexcept
    {
        if (info.media_type == MediaType::Unknown || info.codec == CodecType::Unknown || !info.timestamp.Valid() || !buffer)
        {
            return false;
        }

        if (offset > buffer->size() || size > buffer->size() - offset)
        {
            return false;
        }

        return size > 0;
    }

    uint64_t DurationMs() const noexcept
    {
        if (info.media_type != MediaType::Audio || sample_rate == 0 || sample_count == 0)
        {
            return 0;
        }

        return static_cast<uint64_t>(sample_count) * 1000 / sample_rate;
    }
};


} // namespace media


#endif /* _MEDIA_FRAME_H_ */
