#include "AudioRtpDepacketizerFactory.h"

#include "logger.h"
#include <algorithm>
#include <cctype>
#include <deque>

namespace media
{
namespace
{
std::string Upper(std::string value)
{
    std::transform(value.begin(), value.end(), value.begin(),
                   [](unsigned char c) { return static_cast<char>(std::toupper(c)); });
    return value;
}

EncodedFrame MakePacketFrame(const RtpView& view, CodecType codec,
                             uint32_t sample_rate, uint16_t channels,
                             uint32_t sample_count)
{
    EncodedFrame frame;
    frame.info.media_type = MediaType::Audio;
    frame.info.codec = codec;
    frame.info.timestamp.dts = view.ts;
    frame.info.timestamp.pts = view.ts;
    frame.info.timestamp.time_base_num = 1;
    frame.info.timestamp.time_base_den = sample_rate > 0 ? sample_rate : 1;
    frame.info.integrity = FrameIntegrity::Complete;
    frame.rtp.ssrc = view.ssrc;
    frame.rtp.rtp_timestamp = view.ts;
    frame.rtp.first_sequence = view.seq;
    frame.rtp.last_sequence = view.seq;
    frame.rtp.packet_count = 1;
    frame.frame_type = EncodedFrameType::Audio;
    frame.sample_rate = sample_rate;
    frame.channels = channels;
    frame.sample_count = sample_count;
    frame.buffer = std::make_shared<std::vector<uint8_t>>(
        view.payload, view.payload + view.payload_len);
    frame.size = view.payload_len;
    return frame;
}

class PacketQueueDepacketizer : public IAudioRtpDepacketizer
{
public:
    bool HasFrame() const override { return !frames_.empty(); }

    bool PopFrame(EncodedFrame& out) override
    {
        if (frames_.empty()) return false;
        out = std::move(frames_.front());
        frames_.pop_front();
        return true;
    }

    void Reset() override { frames_.clear(); }

protected:
    std::deque<EncodedFrame> frames_;
};

class G711RtpDepacketizer final : public PacketQueueDepacketizer
{
public:
    G711RtpDepacketizer(CodecType codec, uint32_t sample_rate, uint16_t channels)
        : codec_(codec), sample_rate_(sample_rate), channels_(channels) {}

    bool Input(const RtpView& view) override
    {
        if (!view.valid() || !view.payload || view.payload_len == 0) return false;
        frames_.push_back(MakePacketFrame(view, codec_, sample_rate_, channels_,
                                          static_cast<uint32_t>(view.payload_len)));
        return true;
    }

private:
    CodecType codec_;
    uint32_t sample_rate_;
    uint16_t channels_;
};

// Returns the number of samples represented by one RFC 7587 Opus packet at
// 48 kHz, or zero for a malformed/overlong packet.
uint32_t OpusPacketSampleCount(const uint8_t* data, size_t len)
{
    if (!data || len == 0) return 0;
    const uint8_t toc = data[0];
    const int config = toc >> 3;
    uint32_t samples_per_frame = 0;
    if (config >= 16)
        samples_per_frame = 120U << (config & 3);       // 2.5, 5, 10, 20 ms
    else if (config >= 12)
        samples_per_frame = 480U << (config & 1);      // 10 or 20 ms
    else
        samples_per_frame = 480U << (config & 3);      // 10, 20, 40, 60 ms

    uint32_t frame_count = 1;
    switch (toc & 3)
    {
    case 1:
    case 2: frame_count = 2; break;
    case 3:
        if (len < 2) return 0;
        frame_count = data[1] & 0x3f;
        if (frame_count == 0) return 0;
        break;
    default: break;
    }
    const uint32_t total = samples_per_frame * frame_count;
    return total <= 5760 ? total : 0; // Opus packets are limited to 120 ms.
}

class OpusRtpDepacketizer final : public PacketQueueDepacketizer
{
public:
    OpusRtpDepacketizer(uint32_t sample_rate, uint16_t channels)
        : sample_rate_(sample_rate), channels_(channels) {}

    bool Input(const RtpView& view) override
    {
        if (!view.valid() || !view.payload || view.payload_len == 0) return false;
        const uint32_t samples_48k = OpusPacketSampleCount(view.payload, view.payload_len);
        if (samples_48k == 0) return false;
        const uint32_t samples = sample_rate_ == 48000
                                     ? samples_48k
                                     : static_cast<uint32_t>(
                                           static_cast<uint64_t>(samples_48k) * sample_rate_ / 48000);
        frames_.push_back(MakePacketFrame(view, CodecType::Opus, sample_rate_,
                                          channels_, samples));
        return true;
    }

private:
    uint32_t sample_rate_;
    uint16_t channels_;
};
} // namespace

std::unique_ptr<IAudioRtpDepacketizer> AudioRtpDepacketizerFactory::Create(
    CodecType codec, const std::string& encoding_name, uint32_t sample_rate,
    uint16_t channels, const std::string& fmtp)
{
    const std::string encoding = Upper(encoding_name);
    switch (codec)
    {
    case CodecType::PCMU:
    case CodecType::PCMA:
        LOG_INFO("[AUDIO][FACTORY] selected G711 RTP depacketizer",
                 " encoding=", encoding_name,
                 " sample_rate=", sample_rate,
                 " channels=", channels);
        return std::make_unique<G711RtpDepacketizer>(
            codec, sample_rate, channels);
    case CodecType::Opus:
        LOG_INFO("[AUDIO][FACTORY] selected Opus RTP depacketizer",
                 " sample_rate=", sample_rate,
                 " channels=", channels);
        return std::make_unique<OpusRtpDepacketizer>(sample_rate, channels);
    case CodecType::AAC:
        if (encoding.empty() || encoding == "MPEG4-GENERIC" || encoding == "AAC")
        {
            LOG_INFO("[AUDIO][FACTORY] selected MPEG4-GENERIC RTP depacketizer",
                     " sample_rate=", sample_rate,
                     " channels=", channels);
            return std::make_unique<AudioDepacketizer>(
                codec, sample_rate, channels, fmtp);
        }
        LOG_ERROR("[AudioRtpDepacketizerFactory] unsupported AAC payload format",
                  " encoding=", encoding_name);
        return nullptr;
    default:
        LOG_ERROR("[AudioRtpDepacketizerFactory] unsupported audio codec",
                  " encoding=", encoding_name);
        return nullptr;
    }
}

} // namespace media
