#ifndef _AUDIO_DEPACKETIZER_H_
#define _AUDIO_DEPACKETIZER_H_

#include "Depacketizer.h"
#include "MediaFrame.h"
#include <deque>

namespace media
{

class AudioDepacketizer
{
public:
    AudioDepacketizer(MediaCodecType codec, uint32_t sample_rate, uint32_t channels = 1)
        : codec_(codec),
          sample_rate_(sample_rate),
          channels_(channels)
    {
    }

    bool Input(const RtpView& view);

    bool HasFrame() const;
    bool PopFrame(MediaFrame& out);

private:
    bool InputSimplePayload(const RtpView& view);

private:
    MediaCodecType codec_ = MediaCodecType::Unknown;
    uint32_t sample_rate_ = 0;
    uint32_t channels_ = 1;

    std::deque<MediaFrame> frames_;
};

} // namespace media

#endif /* _AUDIO_DEPACKETIZER_H_ */