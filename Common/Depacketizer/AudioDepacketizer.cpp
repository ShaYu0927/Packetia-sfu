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
    case MediaCodecType::PCMU:
    case MediaCodecType::PCMA:
    case MediaCodecType::Opus:
        return InputSimplePayload(view);

    case MediaCodecType::AAC:
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
    MediaFrame frame;
    frame.type = MediaFrameType::Audio;
    frame.codec = codec_;
    frame.ssrc = view.ssrc;
    frame.timestamp = view.ts;
    frame.first_seq = view.seq;
    frame.last_seq = view.seq;
    frame.sample_rate = sample_rate_;
    frame.channels = channels_;
    frame.complete = true;
    frame.broken = false;

    frame.data.assign(view.payload, view.payload + view.payload_len);

    frames_.push_back(std::move(frame));
    return true;
}

bool AudioDepacketizer::HasFrame() const
{
    return !frames_.empty();
}

bool AudioDepacketizer::PopFrame(MediaFrame& out)
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