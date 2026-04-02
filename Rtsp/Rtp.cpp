#include "Rtp.h"
#include <string>


static inline std::string hex8(uint8_t v)
{
    const char* H = "0123456789ABCDEF";
    std::string s = "00";
    s[0] = H[v >> 4];
    s[1] = H[v & 0x0F];
    return s;
}

void RtpTrack::setInterleavedChannel(uint8_t rtp_channel, uint8_t rtcp_channel)
{
    info_.rtsp_transport.interleaved_rtcp = rtcp_channel;
    info_.rtsp_transport.interleaved_rtp  = rtp_channel;
}

bool AudioTrack::onInputRtp(uint8_t* data, size_t len)
{
    return true;
}

void AudioTrack::onInputRtcp(const uint8_t* data, size_t len)
{
    
}

bool VideoTrack::onInputRtp(uint8_t* data, size_t len)
{
    return true;
}

void VideoTrack::onInputRtcp(const uint8_t* data, size_t len)
{
    
}

bool RtpPacket::assign(const uint8_t* ptr, size_t len,
            uint8_t version,
            bool padding,
            bool extension,
            uint8_t csrcCnt,
            bool marker,
            uint8_t pt,
            uint16_t seq,
            uint32_t timestamp,
            uint32_t ssrc,
            size_t headerLen,
            size_t payloadLen)
{
    if (!ptr || len == 0 || len > capacity_) 
    {
        return false;
    }
    if (headerLen > len || payloadLen > len - headerLen) 
    {
        return false;
    }

    std::memcpy(data_.get(), ptr, len);
    size_ = len;

    version_ = version;
    padding_ = padding;
    extension_ = extension;
    csrc_count_ = csrcCnt;

    marker_ = marker;
    pt_ = pt;
    seq_ = seq;
    ts_ = timestamp;
    ssrc_ = ssrc;

    hdr_len_ = headerLen;
    payload_off_ = headerLen;
    payload_len_ = payloadLen;

    return true;
}


