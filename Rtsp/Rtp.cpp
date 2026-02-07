#include "Rtp.h"
#include "Rtsp.h"
#include <string>

RtpPacket::RtpPacket()
{
}

RtpPacket::~RtpPacket()
{
}

std::shared_ptr<RtpPacket> RtpPacket::create(size_t capacity)
{
    if (capacity == 0) 
    {
        return nullptr;
    }

    auto pkt = std::make_shared<RtpPacket>();
    pkt->capacity = capacity;
    pkt->data = std::shared_ptr<uint8_t[]>(new uint8_t[capacity](), std::default_delete<uint8_t[]>());

    return pkt;
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

uint16_t RtpPacket::getSeq() const
{
    return seq_;
}

RtpPacket::Ptr RtpVideoTracker::inputRtp(TrackType type, int sample_rate, uint8_t *ptr, size_t len)
{
    if (!ptr || len < 12) 
    {
        LOG_ERROR("bad rtp param");
        return nullptr;
    }

    if (sample_rate <= 0) 
    {
        LOG_ERROR("drop rtp: bad sample_rate=0");
        return nullptr;
    }

    if(len < RtpHeader::kSize || ptr == nullptr)
    {
        LOG_ERROR("RTP packet too small and ptr is nullptr, len=" + std::to_string(len));
        return nullptr;
    }

    /* Header alloc */
    auto *w = reinterpret_cast<RtpWireHeader*>(ptr);
    uint8_t v = w->vpxcc >> 6;
    if (v != 2) 
    {
        LOG_ERROR("drop rtp: bad version=" + std::to_string((int)v));
        return nullptr;
    }

    bool x  = (w->vpxcc & 0x10) != 0;
    bool p  = (w->vpxcc & 0x20) != 0; 
    uint8_t cc = (w->vpxcc & 0x0F);

    uint8_t pt = (w->mpt & 0x7F);
    uint16_t seq = ntohs(w->seq);
    uint32_t ts = ntohl(w->ts);
    uint32_t ssrc = ntohl(w->ssrc);

    /* classify + lock PT/SSRC <RFC 3550> */
    size_t header_len = 12 + 4u * cc;
    if (len < header_len) 
    {
        LOG_ERROR("drop rtp: len < csrc header_len, len=" + std::to_string(len));
        return nullptr;
    }

    if (x) 
    {
        // extension header: 16-bit profile + 16-bit length(words)
        if (len < header_len + 4) 
        {
            LOG_ERROR("drop rtp: ext header too small");
            return nullptr;
        }
        const uint8_t *ext = ptr + header_len;
        uint16_t ext_words = (uint16_t(ext[2]) << 8) | uint16_t(ext[3]);
        size_t ext_len = 4 + size_t(ext_words) * 4;
        if (len < header_len + ext_len) 
        {
            LOG_ERROR("drop rtp: ext data truncated");
            return nullptr;
        }
        header_len += ext_len;
    }

    /* padding 如果你要严格处理：payload_end = len - ptr[len-1] */
    if (p) 
    {
        uint8_t pad = ptr[len - 1];
        if (pad == 0 || pad > len) 
        {
            LOG_ERROR("drop rtp: bad padding=" + std::to_string(pad));
            return nullptr;
        }
    }

    /* 解析 RTP 包 */
    auto pkt = RtpPacket::create(len);
    auto dst = pkt ? pkt->data.get() : nullptr;

    LOG_INFO("[inputRtp] after create: seq=", seq,
         " pkt_obj=", (void*)pkt.get(),
         " data_ptr=", (void*)pkt->data.get(),
         " use_count=", pkt.use_count(),
         " len=", len);

#if RTP_DEBUG

    LOG_INFO("pkt=" + std::to_string((uintptr_t)pkt.get()) +
            " dst=" + std::to_string((uintptr_t)dst) +
            " src=" + std::to_string((uintptr_t)ptr) +
            " len=" + std::to_string(len));
#endif

    if (!pkt || !dst) 
    {
        LOG_ERROR("create failed: pkt or pkt->data is null");
        return nullptr;
    }
    memcpy(pkt->data.get(), ptr, len);
    pkt->size = len;
    pkt->type = type;
    pkt->sample_rate = sample_rate;
    pkt->pt = pt;
    pkt->ssrc = ssrc;
    pkt->seq_ = seq;
    
    uint16_t sequence = pkt->getSeq();
    EnhancedPacketSortor<RtpPacket::Ptr, uint16_t>::inputPacket(sequence,pkt);

    return pkt;
}

void RtpVideoTracker::onRtpSorted(RtpPacket::Ptr rtp)
{

#if RTP_DEBUG
    LOG_INFO("[RtpSorted] seq= " + std::to_string(rtp->seq_) + " payload_len= " + std::to_string(rtp->size));
#endif

    if(!rtp)
    {
        LOG_ERROR("error rtp");
        return;
    }

    const uint16_t seq = rtp->getSeq();
    const uint8_t* payload = rtp->getPayload();     
    size_t payload_len = rtp->getPayloadSize();     
    if (!payload || payload_len < 1) return;

    // const uint32_t ts = rtp->getTimestamp();        
    const bool marker = rtp->getMarker();
    
    // if (!has_ts_) 
    // {
    //     has_ts_ = true;
    //     cur_ts_ = ts;
    // } 
    // else if (ts != cur_ts_) 
    // {
    //     emitFrameIfAny(true);
    //     cur_ts_ = ts;
    // }

    const uint8_t nal0 = payload[0];
    const uint8_t nal_type = nal0 & 0x1F;

    if (nal_type >= 1 && nal_type <= 23) 
    {
        append_start_code(nalu_buf_);
        nalu_buf_.insert(nalu_buf_.end(), payload, payload + payload_len);
    }
}

inline void RtpVideoTracker::append_start_code(std::vector<uint8_t> &out)
{
    out.insert(out.end(), {0,0,0,1});
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
    else if(nal_unit_type == 24)  /* // STAP-A类型(聚合包)，检查其中是否包含IDR帧 */
    {
        size_t offset = 1;
        while (offset + 2 < data_size) 
        {
            uint16_t nalu_size = (d[offset] << 8) | d[offset + 1];
            offset += 2;

            if (offset + nalu_size > data_size) 
            {
                break;
            }

            uint8_t stap_nal_unit_type = d[offset] & 0x1F;
            if (stap_nal_unit_type == 5) 
            {
                return true;
            }

            offset += nalu_size;
        }

    }
    else if (nal_unit_type == 28)
    {
        if (data_size < 2) return false;

        const uint8_t fu_ind = d[0];      // F|NRI|28
        const uint8_t fu_hdr = d[1];      // S|E|R|type
        const bool S = (fu_hdr & 0x80) != 0;
        const bool E = (fu_hdr & 0x40) != 0;
        const uint8_t orig_type = fu_hdr & 0x1F;

        const uint8_t F = fu_ind & 0x80;
        const uint8_t NRI = fu_ind & 0x60;
        const uint8_t reconstructed_nal = F | NRI | orig_type;

        const uint8_t* frag = d + 2;
        const size_t frag_len = *d - 2;
    }


    if(pkt->marker)
    {
        /*notfy*/
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

RtpHeader::RtpHeader()
{

}

RtpHeader::~RtpHeader()
{

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

bool RtpHeader::getMarker() const
{
    return false;
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

uint32_t RtpHeader::getSSRC() const
{
    return 0;
}

void RtpHeader::setSSRC(uint32_t ssrc)
{
}


