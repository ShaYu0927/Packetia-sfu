#ifndef AUDIO_RTP_DEPACKETIZER_FACTORY_H
#define AUDIO_RTP_DEPACKETIZER_FACTORY_H

#include "AudioDepacketizer.h"
#include <memory>
#include <string>

namespace media
{

class AudioRtpDepacketizerFactory
{
public:
    static std::unique_ptr<IAudioRtpDepacketizer> Create(
        CodecType codec,
        const std::string& encoding_name,
        uint32_t sample_rate,
        uint16_t channels,
        const std::string& fmtp = {});
};

} // namespace media

#endif
