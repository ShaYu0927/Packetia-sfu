#include "AudioDepacketizer.h"
#include "logger.h"

namespace media
{

bool AudioDepacketizer::Input(const RtpView& view)
{
    if (!view.valid() || !view.payload || view.payload_len == 0)
    {
        LOG_ERROR("[AudioDepacketizer] invalid rtp view",
                  " ssrc=", view.ssrc,
                  " seq=", view.seq,
                  " ts=", view.ts,
                  " payload_len=", view.payload_len);
        return false;
    }

    switch (codec_)
    {
    case CodecType::PCMU:
    case CodecType::PCMA:
    case CodecType::Opus:
        return InputSimplePayload(view);

    case CodecType::AAC:
        LOG_ERROR("[AudioDepacketizer] AAC not supported yet",
                  " seq=", view.seq,
                  " ts=", view.ts);
        return false;

    default:
        LOG_ERROR("[AudioDepacketizer] unsupported codec",
                  " seq=", view.seq,
                  " ts=", view.ts);
        return false;
    }
}

bool AudioDepacketizer::InputSimplePayload(const RtpView& view)
{
    EncodedFrame frame;
    frame.info.media_type = MediaType::Audio;
    frame.info.codec = codec_;
    frame.info.timestamp.dts = view.ts;
    frame.info.timestamp.pts = view.ts;
    frame.info.timestamp.time_base_num = 1;
    frame.info.timestamp.time_base_den = sample_rate_ > 0 ? sample_rate_ : 1;
    frame.info.integrity = FrameIntegrity::Complete;

    frame.rtp.ssrc = view.ssrc;
    frame.rtp.rtp_timestamp = view.ts;
    frame.rtp.first_sequence = view.seq;
    frame.rtp.last_sequence = view.seq;
    frame.rtp.packet_count = 1;

    frame.frame_type = EncodedFrameType::Audio;
    frame.sample_rate = sample_rate_;
    frame.channels = channels_;
    if (codec_ == CodecType::PCMU || codec_ == CodecType::PCMA)
    {
        frame.sample_count = static_cast<uint32_t>(view.payload_len);
    }

    auto buffer = std::make_shared<std::vector<uint8_t>>(view.payload,
                                                         view.payload + view.payload_len);
    frame.buffer = std::move(buffer);
    frame.size = view.payload_len;

    frames_.push_back(std::move(frame));
    return true;
}

bool AudioDepacketizer::HasFrame() const
{
    return !frames_.empty();
}

bool AudioDepacketizer::PopFrame(EncodedFrame& out)
{
    if (frames_.empty())
    {
        return false;
    }

    out = std::move(frames_.front());
    frames_.pop_front();
    return true;
}

} // namespace media
