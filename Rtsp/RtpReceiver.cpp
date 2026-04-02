#include "RtpReceiver.h"
#include "TimeUtil.h"


/* 网络包 转 协议包 */
RtpPacket::Ptr RtpVideoTracker::inputRtp(TrackType type, int sample_rate, uint8_t *ptr, size_t len)
{
    if (!ptr || len < 12)
    {
        LOG_ERROR("inputRtp invalid param, ptr=", (void*)ptr, ", len=", len);
        return nullptr;
    }
      uint8_t vpxcc = ptr[0];
    uint8_t mpt   = ptr[1];

    uint8_t version = (vpxcc >> 6) & 0x03;
    bool padding    = ((vpxcc >> 5) & 0x01) != 0;
    bool extension  = ((vpxcc >> 4) & 0x01) != 0;
    uint8_t csrcCnt = vpxcc & 0x0F;

    bool marker     = ((mpt >> 7) & 0x01) != 0;
    uint8_t pt      = mpt & 0x7F;

    if (version != 2)
    {
        LOG_ERROR("inputRtp invalid rtp version=", (int)version);
        return nullptr;
    }

    size_t headerLen = 12 + csrcCnt * 4;
    if (len < headerLen)
    {
        LOG_ERROR("inputRtp len too short for csrc, len=", len,
                  ", headerLen=", headerLen);
        return nullptr;
    }

    if (extension)
    {
        // RTP extension header:
        // 16bit profile + 16bit length(单位是32bit word)
        if (len < headerLen + 4)
        {
            LOG_ERROR("inputRtp len too short for extension header, len=", len);
            return nullptr;
        }

        uint16_t extLenWords =
            (static_cast<uint16_t>(ptr[headerLen + 2]) << 8) |
             static_cast<uint16_t>(ptr[headerLen + 3]);

        size_t extTotalLen = 4 + static_cast<size_t>(extLenWords) * 4;
        if (len < headerLen + extTotalLen)
        {
            LOG_ERROR("inputRtp len too short for extension body, len=", len,
                      ", need=", headerLen + extTotalLen);
            return nullptr;
        }

        headerLen += extTotalLen;
    }

    if (headerLen > len)
    {
        LOG_ERROR("inputRtp invalid headerLen=", headerLen, ", len=", len);
        return nullptr;
    }

    size_t payloadLen = len - headerLen;

    if (padding)
    {
        if (payloadLen == 0)
        {
            LOG_ERROR("inputRtp invalid padding packet, no payload");
            return nullptr;
        }

        uint8_t padLen = ptr[len - 1];
        if (padLen == 0 || padLen > payloadLen)
        {
            LOG_ERROR("inputRtp invalid padLen=", (int)padLen,
                      ", payloadLen=", payloadLen);
            return nullptr;
        }

        payloadLen -= padLen;
    }

    uint16_t seq =
        (static_cast<uint16_t>(ptr[2]) << 8) |
         static_cast<uint16_t>(ptr[3]);

    uint32_t timestamp =
        (static_cast<uint32_t>(ptr[4]) << 24) |
        (static_cast<uint32_t>(ptr[5]) << 16) |
        (static_cast<uint32_t>(ptr[6]) << 8)  |
         static_cast<uint32_t>(ptr[7]);

    uint32_t ssrc =
        (static_cast<uint32_t>(ptr[8])  << 24) |
        (static_cast<uint32_t>(ptr[9])  << 16) |
        (static_cast<uint32_t>(ptr[10]) << 8)  |
         static_cast<uint32_t>(ptr[11]);

    auto pkt = std::make_shared<RtpPacket>();
    
    if (!pkt->assign(ptr, len,
                 version,
                 padding,
                 extension,
                 csrcCnt,
                 marker,
                 pt,
                 seq,
                 timestamp,
                 ssrc,
                 headerLen,
                 payloadLen))
    {
        LOG_ERROR("assign rtp packet failed");
        return nullptr;
    }

    if (_first_rtp_recv_ms == 0)
    {
        _first_rtp_recv_ms = NowMs();
    }
    _last_rtp_recv_ms = NowMs();

    ++_rtp_packet_count;
    _rtp_bytes += len;

    _ssrc = ssrc;
    _payload_type = pt;
    _last_timestamp = timestamp;

    UpdateSequenceStats(seq);



    /* 排序 */
    inputPacket(pkt);
    return pkt;
}

