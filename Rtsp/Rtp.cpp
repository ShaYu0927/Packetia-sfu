#include "Rtp.h"
#include "Rtsp.h"



RtpHeader::RtpHeader()
{
}

RtpPacket::RtpPacket()
{
}

RtpPacket::Ptr RtpPacket::create(size_t size)
{
    auto pkt = std::make_shared<RtpPacket>();
    pkt->data = std::shared_ptr<uint8_t>(new uint8_t[size], std::default_delete<uint8_t[]>());
    pkt->size = 4;
    return pkt;
}

void RtpHeader::serialize(uint8_t *out) const
{
}

uint8_t RtpHeader::getVersion() const
{
    return 0;
}

void RtpHeader::setVersion(uint8_t ver)
{
}

uint8_t RtpHeader::getPayloadType() const
{
    return 0;
}

void RtpHeader::setPayloadType(uint8_t pt)
{
}



void RtpHeader::setMarker(bool marker)
{
}

uint16_t RtpHeader::getSequence() const
{
    return 0;
}

void RtpHeader::setSequence(uint16_t seq)
{
}

uint32_t RtpHeader::getTimestamp() const
{
    return 0;
}

void RtpHeader::setTimestamp(uint32_t ts)
{
}

void RtpPacket::setSeq(uint16_t seq)
{

}

uint32_t RtpPacket::getStamp() const
{
    return 0;
}

void RtpPacket::setStamp(uint32_t ts)
{
}

uint64_t RtpPacket::getStampMS(bool ntp) const
{
    return 0;
}

uint32_t RtpHeader::getSSRC() const
{
    return 0;
}

void RtpHeader::setSSRC(uint32_t ssrc)
{

}

uint8_t *RtpPacket::getPayload()
{
    return nullptr;
}

size_t RtpPacket::getPayloadSize() const
{
    return size_t();
}

void RtpPacket::setPayload(const uint8_t *payload_data, size_t len)
{

}

bool RtpPacket::getMarker() const
{
    return false;
}

RtpPacket::Ptr RtpVideoTracker::inputRtp(TrackType type, int sample_rate, uint8_t *ptr, size_t len)
{
    // 解析 RTP 包
    RtpPacket::Ptr pkt = RtpPacket::create(len);
    memcpy(pkt->data.get(), ptr, len);
    pkt->size = len;
    pkt->type = type;
    pkt->sample_rate = sample_rate;

    // 当前是否为关键帧
    if (isKeyFrame(pkt)) 
    {
        // 处理缓存的非关键帧

    }


    // 重新拼接视频帧

}

bool RtpVideoTracker::isKeyFrame(const RtpPacket::Ptr &pkt)
{
    if(!pkt || pkt->payload.empty())
    {
        return false;
    }

    uint8_t nal_unit_type = pkt->payload[0] & 0x1F;
    if(nal_unit_type == 5) // IDR帧
    {
        return true;
    }

    // STAP-A类型(聚合包)，检查其中是否包含IDR帧
    if(nal_unit_type == 24)
    {
        size_t offset = 1;
        while(offset + 2 < pkt->payload.size())
        {
            uint16_t nalu_size = (pkt->payload[offset] << 8) | pkt->payload[offset + 1];
            offset += 2;
            if(offset + nalu_size > pkt->payload.size())
            {
                break;
            }
            uint8_t stap_nal_unit_type = pkt->payload[offset] & 0x1F;
            if(stap_nal_unit_type == 5) // IDR帧
            {
                return true;
            }
            offset += nalu_size;
        }
    }

    return false;
}


RtpPacket::Ptr RtpAudioTracker::inputRtp(TrackType type, int sample_rate, uint8_t *ptr, size_t len)
{

}

