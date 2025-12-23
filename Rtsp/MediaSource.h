#ifndef _MEDIASOURCE_H_
#define _MEDIASOURCE_H_

#include <functional>
#include <memory>   // std::shared_ptr, std::unique_ptr
#include <string>
#include "Media.h"

#include "Rtp.h"

class RtpPacket;

class IPacketizer 
{
public:
    using OnRtp = std::function<void(const RtpRawPacket&)>;
    virtual ~IPacketizer() = default;
    virtual void SetOnRtp(OnRtp cb) = 0;
    virtual bool InputFrame(const AVFrame& frame) = 0;
    virtual const RtpTrackInfo& trackInfo() const = 0;
};

class MediaSource 
{
public:
    using Ptr = std::shared_ptr<MediaSource>;
    using OnRtp = std::function<void(MediaChannelId, const RtpRawPacket::Ptr&)>;

    virtual ~MediaSource() = default;

    void SetOnRtp(OnRtp cb) { on_rtp_ = std::move(cb); }

    virtual std::string GetMediaDescription(uint16_t port=0) = 0;
    virtual std::string GetAttribute() = 0;

    virtual bool HandleFrame(MediaChannelId channel_id, const AVFrame& frame) = 0;

protected:
    void emitRtp(MediaChannelId cid, const RtpRawPacket::Ptr& pkt) 
    {
        if (on_rtp_ && pkt) on_rtp_(cid, pkt);
    }

private:
    OnRtp on_rtp_;
};


#endif // _MEDIASOURCE_H_
