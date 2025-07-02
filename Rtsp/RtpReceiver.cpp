#include "RtpReceiver.h"

RtpTrack::RtpTrack()
{
}

uint32_t RtpTrack::getSSRC() const
{
    return 0;
}

//  需要重组RTP数据包，进行排序
RtpPacket::Ptr RtpTrack::inputRtp(TrackType type, int sample_rate, uint8_t *ptr, size_t len)
{
    if(len < RtpPacket::kRtpHeaderSize)
    {
        // RTP包长度小于RTP头部长度，直接丢弃
        LOG_INFO("RtpTrack::inputRtp: RTP packet too short, length: " + std::to_string(len));
        return RtpPacket::Ptr();    
    }

    //如果超过最大包
    if(len > 1024 * RtpPacket::kRtpMaxSize)
    {
        LOG_INFO("RtpTrack::inputRtp: RTP packet too large, length: " + std::to_string(len));
        return RtpPacket::Ptr();
    }

     if (!sample_rate) 
     {
        //无法把时间戳转换成毫秒
        return nullptr;
    }

    //通过内存直接转换为RTPHeader
    RtpHeader* rtp_header = reinterpret_cast<RtpHeader*>(ptr);
    if(rtp_header->getVersion() != RtpPacket::kRtpVersion)
    {
        LOG_INFO("RtpTrack::inputRtp: RTP version mismatch, expected: " + std::to_string(RtpPacket::kRtpVersion) + ", got: " + std::to_string(rtp_header->getVersion()));
        return RtpPacket::Ptr();
    }

    if(rtp_header->getPayloadType() < 0)
    {
        LOG_INFO("RtpTrack::inputRtp: Invalid payload type: " + std::to_string(rtp_header->getPayloadType()));
        return RtpPacket::Ptr();
    }

    //用于区分多个不同 RTP 流，防止混淆
    auto ssrc = ntohl(rtp_header->getSSRC());
    if (_pt == 0xFF) 
    {
        _pt = rtp_header->pt;
    } 
    else if (rtp_header->pt != _pt) 
    {
        //TraceL << "rtp pt mismatch:" << (int) header->pt << " !=" << (int) _pt;
        return nullptr;
    }

    return RtpPacket::Ptr();
}

void RtpTrack::setNtpStamp(uint32_t rtp_stamp, uint64_t ntp_stamp_ms)
{
}
