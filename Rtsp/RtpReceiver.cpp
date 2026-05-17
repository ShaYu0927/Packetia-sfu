#include "RtpReceiver.h"
#include "logger.h"


/* 网络包 转 协议包 */
RtpPacket::Ptr RtpVideoTracker::inputRtp(TrackType type, int sample_rate, uint8_t *ptr, size_t len)
{
    if (!ptr || len < RtpHeader::kSize)
    {
        LOG_ERROR("inputRtp invalid param, ptr=", (void*)ptr, " len=", len);
        return nullptr;
    }

    RtpHeader hdr;
    if (!hdr.InputFromBuffer(ptr, len))
    {
        LOG_ERROR("inputRtp parse rtp header failed");
        return nullptr;
    }

    if (hdr.getVersion() != 2)
    {
        LOG_ERROR("inputRtp invalid rtp version=", static_cast<int>(hdr.getVersion()));
        return nullptr;
    }

    size_t headerLen = hdr.getHeaderSize();
    if (len < headerLen)
    {
        LOG_ERROR("inputRtp len too short for csrc, len=", len, " headerLen=", headerLen);
        return nullptr;
    }

    if (hdr.getExtension())
    {
        if (len < headerLen + 4)
        {
            LOG_ERROR("inputRtp len too short for extension header, len=", len);
            return nullptr;
        }

        uint16_t extLenWords = (static_cast<uint16_t>(ptr[headerLen + 2]) << 8) | static_cast<uint16_t>(ptr[headerLen + 3]);

        size_t extTotalLen = 4 + static_cast<size_t>(extLenWords) * 4;
        if (len < headerLen + extTotalLen)
        {
            LOG_ERROR("inputRtp len too short for extension body, len=", len, " need=", headerLen + extTotalLen);
            return nullptr;
        }

        headerLen += extTotalLen;
    }

    size_t payloadLen = len - headerLen;

    if (hdr.getPadding())
    {
        uint8_t padLen = ptr[len - 1];
        if (padLen == 0 || padLen > payloadLen)
        {
            LOG_ERROR("inputRtp invalid padLen=", static_cast<int>(padLen), " payloadLen=", payloadLen);
            return nullptr;
        }
        payloadLen -= padLen;
    }

    auto pkt = std::make_shared<RtpPacket>();
    pkt->reserve(headerLen + payloadLen);
    pkt->SetSequence(hdr.getSequence());
    pkt->setStamp(hdr.getTimestamp());
    pkt->setSSRC(hdr.getSSRC());
    pkt->setPayloadType(hdr.getPayloadType());
    pkt->setMarker(hdr.getMarker());
    pkt->setPayload(ptr + headerLen, payloadLen);
    pkt->setRaw(ptr, len);


    inputPacket(pkt);
    return pkt;
}


void RtpVideoTracker::onBeforeRtpSorted(const RtpPacket::Ptr &pkt)
{
    if(!pkt)
    {
        LOG_ERROR("RtpPacket is nullptr");
        return;
    }

    auto seq = pkt->getSeq();
    auto ts = pkt->getStamp();
    auto ssrc = pkt->getSSRC();

    if(_has_last_seq)
    {
        uint16_t expected = _last_seq + 1;
        if (seq != expected) 
        {
            LOG_ERROR("[RtpVideoTracker] packet lost after sorted, expected=", expected, " actual=", seq);
        }

    }
    _last_seq = seq;
    _has_last_seq = true;
}

void RtpVideoTracker::onRtpSorted(const RtpPacket::Ptr &pkt)
{
    if (!pkt) 
    {
        LOG_INFO("[RtpVideoTracker] onRtpSorted failed, pkt is null");
        return;
    }

    if (!_depacketizer) 
    {
        LOG_INFO("[RtpVideoTracker] no depacketizer, pt=", pkt->getPayloadType(), " seq=", pkt->getSeq(), " ssrc=", pkt->getSSRC());
        return;
    }

    const uint8_t *payload = pkt->getPayload();
    size_t payload_size = pkt->getPayloadSize();

    if (!payload || payload_size == 0) 
    {
        LOG_INFO("[RtpVideoTracker] empty RTP payload, seq=", pkt->getSeq(), " ssrc=", pkt->getSSRC());
        return;
    }

    RtpView view;
    view.ssrc = pkt->getSSRC();
    view.seq = pkt->getSeq();
    view.ts = pkt->getStamp();
    view.marker = pkt->getMarker();
    view.payload = payload;
    view.payload_len = payload_size;

    if (view.payload && view.payload_len > 0) 
    {
        uint8_t nal_type = view.payload[0] & 0x1F;
    } 

    if (!_depacketizer->input(view)) 
    {
        LOG_ERROR("[RtpVideoTracker] depacketizer input failed, seq=", view.seq, " ts=", view.ts, " ssrc=", view.ssrc, " payload_size=", view.payload_len);
        return;
    }
}
