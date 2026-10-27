#ifndef PACKETIA_MEDIA_STREAM_TRACK_H_
#define PACKETIA_MEDIA_STREAM_TRACK_H_

#include "MediaTypes.h"
#include <cstdint>

namespace media
{
// 协议无关的流内轨道描述，与 TrackMetadata.h 中的业务轨道信息分工不同。
// RTP PT、时钟频率、扩展和 SDP FMTP 由接入层保存，不参与 core 流注册。
struct MediaStreamTrack
{
    TrackId id = 0;
    MediaType type = MediaType::Unknown;
    CodecType codec = CodecType::Unknown;
    // 音频采样率，不是 RTP 时间戳时钟；视频轨道保持为 0。
    uint32_t sample_rate = 0;
    int channels = 0;

    bool Valid() const noexcept
    {
        return (type == MediaType::Audio || type == MediaType::Video) &&
            CodecMediaType(codec) == type &&
            (type != MediaType::Audio || (sample_rate > 0 && channels > 0));
    }
};
}

#endif
