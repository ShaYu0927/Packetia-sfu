#ifndef _MEDIASOURCE_H_
#define _MEDIASOURCE_H_

#include <functional>
#include "Media.h" 

class MediaSource
{
public:
    MediaSource() = default;
    virtual ~MediaSource() ;

    virtual MediaType GetMediaType() const
    { return media_type_; }

    // 从SDP中获取媒体类型
    virtual std::string GetMediaDescription(uint16_t port=0) = 0;
    virtual std::string GetAttribute()  = 0;

    
    virtual bool HanleFrame(MediaSessionId session_id, MediaChannelId channel_id, AVFrame& frame) = 0;
    virtual uint32_t GetPayload() const
    { return payload_; }
    virtual uint32_t GetClockRate() const
    { return clock_rate_; }


private:
    MediaType media_type_ = NONE;
	uint32_t  payload_    = 0;
	uint32_t  clock_rate_ = 0;


};  


#endif // _MEDIASOURCE_H_