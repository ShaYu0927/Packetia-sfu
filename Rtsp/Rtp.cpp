#include "Rtp.h"
#include "Rtsp.h"
#include <string>

static inline std::string hex8(uint8_t v)
{
    const char* H = "0123456789ABCDEF";
    std::string s = "00";
    s[0] = H[v >> 4];
    s[1] = H[v & 0x0F];
    return s;
}

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
    seq_ = seq;
}

uint32_t RtpPacket::getStamp() const
{
    return recv_time_ms;
}


uint64_t RtpPacket::getStampMS(bool ntp) const
{
    return ntp_stamp_ms;
}


uint8_t *RtpPacket::getPayload()
{
    return data.get();
}

size_t RtpPacket::getPayloadSize() const
{
    return data ? size : 0;
}

void RtpPacket::setPayload(const uint8_t *payload_data, size_t len)
{
    if (payload_data && len > 0 && len <= capacity) 
    {
        memcpy(data.get(), payload_data, len);
        size = len;
    } 
    else 
    {
        size = 0;
    }
}

bool RtpPacket::getMarker() const
{
    return marker;
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
    size_t header_len = 12 + 4u * cc;

    if (len < header_len) 
    {
        LOG_ERROR("drop rtp: len < csrc header_len, len=" + std::to_string(len));
        return nullptr;
    }

    if (x)
    {
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

    /* padding: payload_end = len - ptr[len-1] */
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

#if RTP_DEBUG
    LOG_INFO("[inputRtp] after create: seq=", seq,
         " pkt_obj=", (void*)pkt.get(),
         " data_ptr=", (void*)pkt->data.get(),
         " use_count=", pkt.use_count(),
         " len=", len);
#endif

#if 1

    LOG_INFO(std::string("[RTP] hdr v=") + std::to_string((int)v) +
         " pt=" + std::to_string((int)pt) +
         " m=" + std::to_string((int)((w->mpt & 0x80) != 0)) +
         " seq=" + std::to_string(seq) +
         " ts=" + std::to_string(ts) +
         " ssrc=" + std::to_string(ssrc) +
         " cc=" + std::to_string((int)cc) +
         " x=" + std::to_string((int)x) +
         " p=" + std::to_string((int)p) +
         " hdr_len=" + std::to_string(header_len) +
         " total_len=" + std::to_string(len) +
         " payload_len=" + std::to_string(len - header_len));
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
    pkt->marker = (w->mpt & 0x80) != 0;
    pkt->pt = pt;
    pkt->ssrc = ssrc;
    pkt->seq_ = seq;
    pkt->ts = ts;
    pkt->version = v;
    pkt->padding = p;
    pkt->extension = x;
    pkt->cc = cc;
    pkt->csrc_count = cc;
    pkt->hdr_len = header_len;
    pkt->payload_off = header_len;
    pkt->payload_len = len - header_len;
    pkt->recv_time_ms = static_cast<uint32_t>(std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count());
    
    uint16_t sequence = pkt->getSeq();
    EnhancedPacketSortor<RtpPacket::Ptr, uint16_t>::inputPacket(sequence,pkt);
    return pkt;
}

void RtpVideoTracker::inputRtcp(const uint8_t* ptr, size_t len)
{
    uint8_t v  = (ptr[0] >> 6) & 0x03;
    uint8_t p  = (ptr[0] >> 5) & 0x01;
    uint8_t rc = (ptr[0] & 0x1F);
    uint8_t pt = ptr[1];
    uint16_t length_words = (ptr[2] << 8) | ptr[3];
    size_t pkt_len = (length_words + 1) * 4;

    LOG_INFO("[RtpVideoTracker] inputRtcp: len= v= p= rc= pt= length_words= pkt_len=",
         len, v, p, rc, pt, length_words, pkt_len);
    rtcp_packet_->OnRtcpPacket(ptr,len);
}

void RtpVideoTracker::onRtpSorted(RtpPacket::Ptr rtp)
{
    if (!rtp) 
    {
        LOG_ERROR("error rtp");
        return;
    }


    uint8_t* base = rtp->data.get();
    size_t off = rtp->payload_off;
    size_t paylen = rtp->payload_len;

    uint8_t b0 = base ? base[0] : 0;
    uint8_t p0 = (base && off < rtp->size) ? base[off] : 0;
    uint8_t nal_type = p0 & 0x1F;

    LOG_INFO(std::string("[onRtpSorted] seq=") + std::to_string(rtp->seq_) +
             " ts=" + std::to_string(rtp->ts) +
             " ssrc=" + std::to_string(rtp->ssrc) +
             " m=" + std::to_string((int)rtp->getMarker()) +
             " size=" + std::to_string(rtp->size) +
             " hdr_len=" + std::to_string(rtp->hdr_len) +
             " payload_off=" + std::to_string(off) +
             " payload_len=" + std::to_string(paylen) +
             " base0=0x" + hex8(b0) +
             " pay0=0x" + hex8(p0) +
             " nal_type=" + std::to_string((int)nal_type));

    RtpView v;
    v.seq = rtp->getSeq();
    v.ts = rtp->getStamp();
    v.ssrc = rtp->ssrc;                  
    v.marker = rtp->getMarker();

    v.payload = base + off;
    v.payload_len = paylen;

    depacketizer_->input(v);
}


void RtpVideoTracker::OnReceiverReport(uint32_t sender_ssrc, const std::vector<rtcpx::RrBlock>& blocks)
{

}
void RtpVideoTracker::OnPli(uint32_t media_ssrc)
{

}
void RtpVideoTracker::OnNack(uint32_t media_ssrc, const std::vector<uint16_t>& missing_seq)
{

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


void RtpHeader::serialize(uint8_t *out) const
{
}

uint8_t RtpHeader::getVersion() const
{
    return _version;
}

void RtpHeader::setVersion(uint8_t ver)
{
    _version = ver;
}

uint8_t RtpHeader::getPayloadType() const
{
    return _payload_type;
}

void RtpHeader::setPayloadType(uint8_t pt)
{
    _payload_type = pt;
}

bool RtpHeader::getMarker() const
{
    return _marker;
}

void RtpHeader::setMarker(bool marker)
{
    _marker = marker;
}

uint16_t RtpHeader::getSequence() const
{
    return _seq;
}

void RtpHeader::setSequence(uint16_t seq)
{
    _seq = seq;
}

uint32_t RtpHeader::getSSRC() const
{
    return _ssrc;
}

void RtpHeader::setSSRC(uint32_t ssrc)
{
    _ssrc = ssrc;
}

void RtpHeader::setTimestamp(uint32_t ts)
{

}


