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
}

bool RtpVideoTracker::isKeyFrame(const RtpPacket::Ptr &pkt)
{
    return false;
}


RtpPacket::Ptr RtpAudioTracker::inputRtp(TrackType type, int sample_rate, uint8_t *ptr, size_t len)
{
}

