#ifndef PACKETIA_VIDEO_RTP_DEPACKETIZER_FACTORY_H_
#define PACKETIA_VIDEO_RTP_DEPACKETIZER_FACTORY_H_

#include "VideoRtpDepacketizer.h"
#include <memory>
#include <string>

namespace media
{

class VideoRtpDepacketizerFactory
{
public:
    // Unsupported codecs/payload formats return nullptr; never fall back to H264.
    static std::unique_ptr<IVideoRtpDepacketizer> Create(
        CodecType codec, const std::string& encoding_name,
        uint32_t clock_rate, const std::string& fmtp = {});
};

} // namespace media

#endif
