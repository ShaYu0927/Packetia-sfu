#ifndef PACKETIA_VIDEO_RTP_DEPACKETIZER_H_
#define PACKETIA_VIDEO_RTP_DEPACKETIZER_H_

#include "Depacketizer.h"
#include "MediaFrame.h"

namespace media
{

// Receives packets in arrival order. Each codec owns packet reordering,
// access-unit boundaries, loss recovery and its encoded payload format.
class IVideoRtpDepacketizer
{
public:
    virtual ~IVideoRtpDepacketizer() = default;
    virtual bool Input(const RtpView& packet) = 0;
    virtual bool HasFrame() const = 0;
    virtual bool PopFrame(EncodedFrame& frame) = 0;
    // Called on the same media owner during idle periods as well as input.
    // True means a pending frame chain expired and needs a recovery keyframe.
    virtual bool FlushExpired() { return false; }
    // Drop stream state and restore the negotiated codec configuration.
    virtual void Reset() = 0;
};

} // namespace media

#endif
