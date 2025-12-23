#include "Rtp.h"
#include "Rtsp.h"



RtpHeader::RtpHeader()
{
}

RtpPacket::RtpPacket()
{
}

RtpPacket::~RtpPacket()
{
    fprintf(stderr, "RtpPacket dtor this=%p\n", this);
}

RtpPacket::Ptr RtpPacket::create(size_t capacity)
{
    auto pkt = std::make_shared<RtpPacket>();

    pkt->data = std::shared_ptr<uint8_t>(
        new uint8_t[capacity],
        std::default_delete<uint8_t[]>()
    );

    pkt->size = 0;
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
    if (!ptr || len < 12 || len > 2000) {
        LOG_ERROR("bad rtp param");
        abort();
    }

    if(len < RtpHeader::kSize || ptr == nullptr)
    {
        LOG_ERROR("RTP packet too small and ptr is nullptr, len=" + std::to_string(len));
        return nullptr;
    }
    /* 解析 RTP 包 */
    auto pkt = RtpPacket::create(len);
    printf("pkt use_count=%ld\n", (long)pkt.use_count());
    auto dst = pkt ? pkt->data.get() : nullptr;

    LOG_INFO("pkt=" + std::to_string((uintptr_t)pkt.get()) +
            " dst=" + std::to_string((uintptr_t)dst) +
            " src=" + std::to_string((uintptr_t)ptr) +
            " len=" + std::to_string(len));

    if (!pkt || !dst) 
    {
        LOG_ERROR("create failed: pkt or pkt->data is null");
        return nullptr;
    }
    memcpy(pkt->data.get(), ptr, len);
    pkt->size = len;
    pkt->type = type;
    pkt->sample_rate = sample_rate;
    return pkt;
}

bool RtpVideoTracker::isKeyFrame(const RtpPacket::Ptr &pkt)
{
    if(!pkt)
    {
        return false;
    }

    uint8_t* d = pkt->data.get();
    size_t data_size = pkt->size;

    uint8_t nal_unit_type = d[0] & 0x1F;
    if(nal_unit_type == 5) // IDR帧
    {
        return true;
    }

    // STAP-A类型(聚合包)，检查其中是否包含IDR帧
    if(nal_unit_type == 24)
    {
        size_t offset = 1;
        while (offset + 2 < data_size) 
        {
            uint16_t nalu_size = (d[offset] << 8) | d[offset + 1];
            offset += 2;

            if (offset + nalu_size > data_size) {
                break;
            }

            uint8_t stap_nal_unit_type = d[offset] & 0x1F;
            if (stap_nal_unit_type == 5) {
                return true;
            }

            offset += nalu_size;
        }

    }

    return false;
}


RtpPacket::Ptr RtpAudioTracker::inputRtp(TrackType type, int sample_rate, uint8_t *ptr, size_t len)
{
    (void)type;
    (void)sample_rate;
    (void)ptr;
    (void)len;
    return nullptr;
}

