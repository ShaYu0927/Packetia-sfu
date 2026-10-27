#ifndef PACKETIA_MEDIA_TYPES_H_
#define PACKETIA_MEDIA_TYPES_H_

#include <cstdint>

namespace media
{
using TrackId = uint32_t;
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
