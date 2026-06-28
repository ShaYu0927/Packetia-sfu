#include "RtpReceiver.h"
#include "logger.h"

#include <chrono>

namespace rtsp 
{

namespace
{
uint64_t NowMs()
{
    using namespace std::chrono;
    return duration_cast<milliseconds>(steady_clock::now().time_since_epoch()).count();
}

uint16_t SeqDistance(uint16_t newer, uint16_t older)
{
    return static_cast<uint16_t>(newer - older);
}

bool SeqOlderThan(uint16_t seq, uint16_t expected)
{
    return seq != expected && SeqDistance(seq, expected) >= 0x8000;
}

const char* BoolText(bool value)
{
    return value ? "1" : "0";
}
}

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
    pkt->reserve(len);
    pkt->SetSequence(hdr.getSequence());
    pkt->setStamp(hdr.getTimestamp());
    pkt->setSSRC(hdr.getSSRC());
    pkt->setPayloadType(hdr.getPayloadType());
    pkt->setMarker(hdr.getMarker());
    pkt->setTrackType(type);
    pkt->setSampleRate(sample_rate > 0 ? static_cast<uint32_t>(sample_rate) : _info.clock_rate);
    pkt->setTrackIndex(_info.track_index);
    pkt->setRaw(ptr, len);
    pkt->setHeaderInfo(headerLen, headerLen, payloadLen);
    pkt->setRecvTimeMs(NowMs());

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
        return;
    }
}

RtpAudioTracker::RtpAudioTracker(const TrackInfo& info)
    : RtpReceiverTrack(info)
{
    media::MediaCodecType codec = media::MediaCodecType::Unknown;

    switch (info.codec_id)
    {
    case CodecId::PCMU:
        codec = media::MediaCodecType::PCMU;
        break;

    case CodecId::PCMA:
        codec = media::MediaCodecType::PCMA;
        break;

    case CodecId::OPUS:
        codec = media::MediaCodecType::Opus;
        break;

    case CodecId::AAC:
        codec = media::MediaCodecType::AAC;
        break;

    default:
        codec = media::MediaCodecType::Unknown;
        break;
    }

    uint32_t sample_rate = info.clock_rate > 0 ? info.clock_rate : 8000;
    uint32_t channels = info.channels > 0 ? static_cast<uint32_t>(info.channels) : 1;

    depacketizer_ = std::make_unique<media::AudioDepacketizer>(codec, sample_rate, channels);
}

RtpPacket::Ptr RtpAudioTracker::inputRtp(TrackType type, int sample_rate, uint8_t* ptr, size_t len)
{
    if (!ptr || len < RtpHeader::kSize)
    {
        return nullptr;
    }

    RtpHeader hdr;
    if (!hdr.InputFromBuffer(ptr, len))
    {
        return nullptr;
    }

    if (hdr.getVersion() != 2)
    {
        return nullptr;
    }


    size_t header_len = hdr.getHeaderSize();

    if (len < header_len)
    {
        return nullptr;
    }

    if (hdr.getExtension())
    {
        if (len < header_len + 4)
        {
            return nullptr;
        }

        uint16_t ext_words = (static_cast<uint16_t>(ptr[header_len + 2]) << 8) | static_cast<uint16_t>(ptr[header_len + 3]);
        size_t ext_total_len = 4 + static_cast<size_t>(ext_words) * 4;

        if (len < header_len + ext_total_len)
        {
            return nullptr;
        }

        header_len += ext_total_len;
    }

    size_t payload_len = len - header_len;

    if (hdr.getPadding())
    {
        uint8_t pad_len = ptr[len - 1];

        if (pad_len == 0 || pad_len > payload_len)
        {
            return nullptr;
        }

        payload_len -= pad_len;
    }

    if (payload_len == 0)
    {
        LOG_ERROR("[RtpAudioTracker] empty payload",
                  " seq=", hdr.getSequence(),
                  " ssrc=", hdr.getSSRC(),
                  " len=", len,
                  " header_len=", header_len);
        return nullptr;
    }

    LOG_INFO("[RtpAudioTracker] RTP payload ready",
             " seq=", hdr.getSequence(),
             " ts=", hdr.getTimestamp(),
             " ssrc=", hdr.getSSRC(),
             " pt=", static_cast<int>(hdr.getPayloadType()),
             " header_len=", header_len,
             " payload_len=", payload_len);

    auto pkt = std::make_shared<RtpPacket>();

    if (!pkt->reserve(len))
    {
        LOG_ERROR("[RtpAudioTracker] reserve failed",
                  " len=", len,
                  " seq=", hdr.getSequence(),
                  " ssrc=", hdr.getSSRC());
        return nullptr;
    }

    pkt->SetSequence(hdr.getSequence());
    pkt->setStamp(hdr.getTimestamp());
    pkt->setSSRC(hdr.getSSRC());
    pkt->setPayloadType(hdr.getPayloadType());
    pkt->setMarker(hdr.getMarker());

    pkt->setTrackType(type);
    pkt->setSampleRate(sample_rate > 0 ? static_cast<uint32_t>(sample_rate)
                                       : _info.clock_rate);
    pkt->setTrackIndex(_info.track_index);

    pkt->setRaw(ptr, len);
    pkt->setHeaderInfo(header_len, header_len, payload_len);
    pkt->setRecvTimeMs(NowMs());

    LOG_INFO("[RtpAudioTracker] push to sorter",
             " this=", this,
             " seq=", pkt->getSeq(),
             " ts=", pkt->getStamp(),
             " ssrc=", pkt->getSSRC(),
             " pt=", static_cast<int>(pkt->getPayloadType()),
             " payload_size=", pkt->getPayloadSize());

    inputPacket(pkt);

    LOG_INFO("[RtpAudioTracker] inputRtp leave",
             " this=", this,
             " seq=", pkt->getSeq(),
             " ssrc=", pkt->getSSRC());

    return pkt;

}

void RtpAudioTracker::inputRtcp(const uint8_t* ptr, size_t len)
{

}

void RtpAudioTracker::onRtpSorted(const RtpPacket::Ptr& pkt)
{

}


void RtcpDispatcher::OnNack(uint32_t sender_ssrc, uint32_t media_ssrc, const uint16_t* seqs, size_t count)
{
    if(!seqs || count == 0)
    {
        return;
    }

    auto it = send_tracks_.find(media_ssrc);

    if (it == send_tracks_.end()) 
    {
        LOG_INFO("[RTCP][NACK] sender track not found", " media_ssrc=", media_ssrc, " count=", count);
        return;
    }

    auto track = it->second.lock();
    if(!track)
    {
        LOG_INFO("[RTCP][NACK] sender track expired", " media_ssrc=", media_ssrc, " count=", count);
        return;
    }

    std::vector<uint16_t> lost_seqs(seqs, seqs + count);
    LOG_INFO("[RTCP][NACK] retransmit request", " media_ssrc=", media_ssrc, " count=", lost_seqs.size());

    track->OnRtcpNack(lost_seqs);
}

}
