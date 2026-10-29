#ifndef PACKETIA_MEDIA_TYPES_H_
#define PACKETIA_MEDIA_TYPES_H_

#include <cstdint>

namespace media
{
using TrackId = uint32_t;
// stream_id 标识本次发布实例；track_id 只在该流内唯一，0 也是有效轨道。
struct MediaTrackRef
{
    uint32_t stream_id = 0;
    TrackId track_id = 0;
    bool Valid() const noexcept { return stream_id != 0; }
    bool operator==(const MediaTrackRef& other) const noexcept
    { return stream_id == other.stream_id && track_id == other.track_id; }
    bool operator!=(const MediaTrackRef& other) const noexcept { return !(*this == other); }
};
enum class MediaType { Unknown = 0, Audio, Video, Data };
// 保持已有编码枚举的数值，新增格式追加到末尾。
enum class CodecType { Unknown = 0, H264, H265, Opus, PCMU, PCMA, AAC, VP8, VP9, AV1 };
enum class MediaFlowDirection { Unknown = 0, Inbound, Outbound };

constexpr MediaType CodecMediaType(CodecType codec) noexcept
{
    switch (codec)
    {
    case CodecType::Opus:
    case CodecType::PCMU:
    case CodecType::PCMA:
    case CodecType::AAC: return MediaType::Audio;
    case CodecType::H264:
    case CodecType::H265:
    case CodecType::VP8:
    case CodecType::VP9:
    case CodecType::AV1: return MediaType::Video;
    default: return MediaType::Unknown;
    }
}
}

#endif
