#ifndef _MEDIA_FRAME_H_
#define _MEDIA_FRAME_H_

#include <cstdint>
#include <vector>

namespace media
{

enum class MediaFrameType
{
    Unknown = 0,
    Audio,
    Video
};

enum class MediaCodecType
{
    Unknown = 0,
    H264,
    H265,
    Opus,
    PCMU,
    PCMA,
    AAC
};

struct MediaFrame
{
    MediaFrameType type = MediaFrameType::Unknown;
    MediaCodecType codec = MediaCodecType::Unknown;

    uint32_t ssrc = 0;
    uint32_t timestamp = 0;
    uint16_t first_seq = 0;
    uint16_t last_seq = 0;

    uint32_t sample_rate = 0;
    uint32_t channels = 1;

    bool keyframe = false;  
    bool complete = true;
    bool broken = false;

    std::vector<uint8_t> data;

    uint32_t DurationMs() const
    {
        if (sample_rate == 0 || data.empty())
        {
            return 0;
        }

        if (codec == MediaCodecType::PCMU || codec == MediaCodecType::PCMA)
        {
            return static_cast<uint32_t>((data.size() * 1000) / sample_rate);
        }

        return 0;
    }
};

} // namespace media


#endif /* _MEDIA_FRAME_H_ */