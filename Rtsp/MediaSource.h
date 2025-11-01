#ifndef _MEDIASOURCE_H_
#define _MEDIASOURCE_H_

#include <functional>
#include <memory>   // std::shared_ptr, std::unique_ptr
#include <string>
#include "Media.h"

#include "Rtp.h"

class RtpPacket;

class MediaSource
{
public:
    using Ptr = std::unique_ptr<MediaSource>;
    using SendFrameCallback = std::function<bool(MediaChannelId channel_id, std::shared_ptr<RtpPacket> pkt)>;

    MediaSource() = default;
    virtual ~MediaSource() = default;

    virtual MediaType GetMediaType() const { return media_type_; }

    virtual std::string GetMediaDescription(uint16_t port=0) = 0;
    virtual std::string GetAttribute()  = 0;
    
    virtual bool HanleFrame(MediaChannelId channel_id, const AVFrame &frame) = 0;
    virtual uint32_t GetPayload() const { return payload_; }
    virtual uint32_t GetClockRate() const { return clock_rate_; }

protected:
    MediaType media_type_ = NONE;
    uint32_t payload_ = 0;
    uint32_t clock_rate_ = 0;
    SendFrameCallback send_frame_callback_;
};

#endif // _MEDIASOURCE_H_
