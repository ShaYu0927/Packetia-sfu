#ifndef PACKETIA_MEDIA_RTP_STREAM_CONTEXT_H_
#define PACKETIA_MEDIA_RTP_STREAM_CONTEXT_H_

#include "MediaTypes.h"

#include <cstdint>
#include <string>
#include <vector>

namespace media
{
// 一个媒体段中的候选 RTP 编码；clock_rate 是 RTP 时钟，不是通用音频采样率。
struct RtpPayloadInfo
{
    uint8_t payload_type = 0;
    CodecType codec = CodecType::Unknown;
    int clock_rate = 0;
    int channels = 0;
    std::string codec_name;
    std::string fmtp;
};

struct RtpTrackContext
{
    int media_index = -1;
    MediaType media_type = MediaType::Unknown;
    uint8_t transport_cc_extension_id = 0;
    std::vector<RtpPayloadInfo> payloads;
};

struct RtpStreamContext
{
    std::vector<RtpTrackContext> tracks;
};

}

#endif // PACKETIA_MEDIA_RTP_STREAM_CONTEXT_H_
