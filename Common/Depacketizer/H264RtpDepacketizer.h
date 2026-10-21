#ifndef PACKETIA_H264_RTP_DEPACKETIZER_H_
#define PACKETIA_H264_RTP_DEPACKETIZER_H_

#include "H264Depacketizer.h"
#include "VideoRtpDepacketizer.h"

namespace media
{

// Converts H264 access units to owning Annex B frames.
class H264RtpDepacketizer final : public IVideoRtpDepacketizer
{
public:
    explicit H264RtpDepacketizer(const std::string& fmtp = {});
    bool Input(const RtpView& packet) override;
    bool HasFrame() const override;
    bool PopFrame(EncodedFrame& frame) override;
    void Reset() override;
    bool FlushExpired() override { return depacketizer_.FlushExpired(); }

private:
    H264Depacketizer depacketizer_;
};

} // namespace media

#endif
