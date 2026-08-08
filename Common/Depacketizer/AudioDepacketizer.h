#ifndef _AUDIO_DEPACKETIZER_H_
#define _AUDIO_DEPACKETIZER_H_

#include "Depacketizer.h"
#include "MediaFrame.h"
#include <deque>
#include <string>

namespace media
{

class IAudioRtpDepacketizer
{
public:
    virtual ~IAudioRtpDepacketizer() = default;
    virtual bool Input(const RtpView& view) = 0;
    virtual bool HasFrame() const = 0;
    virtual bool PopFrame(EncodedFrame& out) = 0;
    virtual void Reset() = 0;
};


class AudioDepacketizer : public IAudioRtpDepacketizer
{
public:
    AudioDepacketizer(CodecType codec, uint32_t sample_rate, uint16_t channels = 1, const std::string& fmtp = {})
        : codec_(codec),
          sample_rate_(sample_rate),
          channels_(channels)
    {
        ParseAacFmtp(fmtp);
    }

    bool Input(const RtpView& view) override;

    bool HasFrame() const override;
    bool PopFrame(EncodedFrame& out) override;
    void Reset() override;

private:
    bool InputSimplePayload(const RtpView& view);
    bool InputAac(const RtpView& view);
    void ParseAacFmtp(const std::string& fmtp);
    void EmitAacFrame(const uint8_t* data,  size_t len, uint32_t timestamp, uint32_t ssrc, uint16_t first_seq, uint16_t last_seq);
    void ResetAacFragment();

    struct AacPayloadConfig
    {
        uint8_t size_length = 13;
        uint8_t index_length = 3;
        uint8_t index_delta_length = 3;
        uint8_t cts_delta_length = 0;
        uint8_t dts_delta_length = 0;
        bool random_access_indication = false;
        uint8_t stream_state_indication = 0;
        uint32_t constant_size = 0;
        uint32_t constant_duration = 1024;
    };

    struct AacFragment
    {
        bool active = false;
        uint32_t ssrc = 0;
        uint32_t timestamp = 0;
        uint32_t expected_size = 0;
        uint16_t first_seq = 0;
        uint16_t last_seq = 0;
        std::vector<uint8_t> data;
    };

private:
    CodecType codec_ = CodecType::Unknown;
    uint32_t sample_rate_ = 0;
    uint16_t channels_ = 1;
    AacPayloadConfig aac_config_;
    AacFragment aac_fragment_;

    std::deque<EncodedFrame> frames_;
};

} // namespace media

#endif /* _AUDIO_DEPACKETIZER_H_ */
