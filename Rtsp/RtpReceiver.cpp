#include "RtpReceiver.h"
#include "logger.h"

#include <algorithm>
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

    auto pkt = acquirePacket(len);
    if (!pkt)
    {
        return nullptr;
    }
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

    if (!inputPacket(pkt))
    {
        return nullptr;
    }
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
        return;
    }

    if (!_depacketizer) 
    {
        return;
    }

    if(_nack_receiver)
    {
        const uint64_t now_ms = pkt->getRecvTimeMs();

        _nack_receiver->OnReceivedPacket(pkt->getSeq(), now_ms);
        _nack_receiver->Process(now_ms);
    }

    const uint8_t *payload = pkt->getPayload();
    size_t payload_size = pkt->getPayloadSize();

    if (!payload || payload_size == 0) 
    {
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

    media::H264AccessUnit au;
    while (_depacketizer->popAccessUnit(au))
    {
        auto frame = std::make_shared<media::EncodedFrame>();
        frame->info.track_id = _info.track_index >= 0 ? static_cast<media::TrackId>(_info.track_index) : 0;
        frame->info.media_type = media::MediaType::Video;
        frame->info.codec = media::CodecType::H264;
        frame->info.timestamp.dts = au.timestamp;
        frame->info.timestamp.pts = au.timestamp;
        frame->info.timestamp.time_base_num = 1;
        frame->info.timestamp.time_base_den = pkt->getSampleRate() > 0 ? pkt->getSampleRate() : 90000;
        frame->info.timestamp.capture_time_ms = pkt->getRecvTimeMs();
        frame->info.integrity = au.broken
                                    ? media::FrameIntegrity::Corrupted
                                    : (au.complete ? media::FrameIntegrity::Complete
                                                   : media::FrameIntegrity::Incomplete);

        frame->rtp.ssrc              = au.ssrc;
        frame->rtp.rtp_timestamp     = au.timestamp;
        frame->rtp.first_sequence    = au.first_seq;
        frame->rtp.last_sequence     = au.last_seq;
        frame->rtp.packet_count      = static_cast<uint16_t>(au.last_seq - au.first_seq) + 1;

        frame->frame_type            = au.keyframe ? media::EncodedFrameType::Key : media::EncodedFrameType::Delta;
        frame->sample_rate           = pkt->getSampleRate();
        auto buffer                  = std::make_shared<std::vector<uint8_t>>(au.ToAnnexB());
        frame->buffer                = std::move(buffer);
        frame->size                  = frame->buffer->size();
    }
}

RtpAudioTracker::RtpAudioTracker(const TrackInfo& info)
    : RtpReceiverTrack(info)
{
    media::CodecType codec = media::CodecType::Unknown;

    switch (info.codec_id)
    {
    case CodecId::PCMU:
        codec = media::CodecType::PCMU;
        break;

    case CodecId::PCMA:
        codec = media::CodecType::PCMA;
        break;

    case CodecId::OPUS:
        codec = media::CodecType::Opus;
        break;

    case CodecId::AAC:
        codec = media::CodecType::AAC;
        break;

    default:
        codec = media::CodecType::Unknown;
        break;
    }

    uint32_t sample_rate  = info.clock_rate > 0 ? info.clock_rate : 8000;
    uint32_t channels     = info.channels > 0 ? static_cast<uint32_t>(info.channels) : 1;
    depacketizer_         = std::make_unique<media::AudioDepacketizer>(codec,
                                                                       sample_rate,
                                                                       channels,
                                                                       info.fmtp);
}

RtpAudioTracker::Ptr RtpAudioTracker::Clone()
{
    auto clone = std::make_shared<RtpAudioTracker>(_info);

    std::lock_guard<std::mutex> lock(clones_mutex_);
    clones_.erase(
        std::remove_if(clones_.begin(), clones_.end(),
                       [](const std::weak_ptr<RtpAudioTracker>& item) {
                           return item.expired();
                       }),
        clones_.end());
    clones_.push_back(clone);
    return clone;
}

std::vector<RtpAudioTracker::Ptr> RtpAudioTracker::SnapshotClones()
{
    std::vector<Ptr> result;
    std::lock_guard<std::mutex> lock(clones_mutex_);

    auto it = clones_.begin();
    while (it != clones_.end())
    {
        if (auto clone = it->lock())
        {
            result.push_back(std::move(clone));
            ++it;
        }
        else
        {
            it = clones_.erase(it);
        }
    }

    return result;
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
        return nullptr;
    }

    auto pkt = acquirePacket(len);
    if (!pkt)
    {
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

    if (!inputPacket(pkt))
    {
        return nullptr;
    }

    for (const auto& clone : SnapshotClones())
    {
        clone->inputRtp(type, sample_rate, ptr, len);
    }

    return pkt;

}

void RtpAudioTracker::onRtpSorted(const RtpPacket::Ptr& pkt)
{
    if (!pkt || !depacketizer_)
    {
        return;
    }

    const uint8_t* payload = pkt->getPayload();
    const size_t payload_size = pkt->getPayloadSize();
    if (!payload || payload_size == 0)
    {
        return;
    }

    RtpView view;
    view.ssrc = pkt->getSSRC();
    view.seq = pkt->getSeq();
    view.ts = pkt->getStamp();
    view.marker = pkt->getMarker();
    view.payload = payload;
    view.payload_len = payload_size;

    if (!depacketizer_->Input(view))
    {
        return;
    }

    media::EncodedFrame completed;
    while (depacketizer_->PopFrame(completed))
    {
        completed.info.track_id = _info.track_index >= 0
                                      ? static_cast<media::TrackId>(_info.track_index)
                                      : 0;
        completed.info.timestamp.capture_time_ms = pkt->getRecvTimeMs();
        emitEncodedFrame(std::make_shared<media::EncodedFrame>(std::move(completed)));
    }

}


void RtcpDispatcher::AddReceiverTrack(uint32_t media_ssrc, std::weak_ptr<RtpReceiverTrack> track)
{
    std::lock_guard<std::mutex> lock(tracks_mutex_);
    recv_tracks_[media_ssrc] = std::move(track);
}

void RtcpDispatcher::RemoveReceiverTrack(uint32_t media_ssrc)
{
    std::lock_guard<std::mutex> lock(tracks_mutex_);
    recv_tracks_.erase(media_ssrc);
}

void RtcpDispatcher::AddSenderTrack(uint32_t media_ssrc, std::weak_ptr<RtpSenderTrack> track)
{
    std::lock_guard<std::mutex> lock(tracks_mutex_);
    send_tracks_[media_ssrc] = std::move(track);
}

void RtcpDispatcher::RemoveSenderTrack(uint32_t media_ssrc)
{
    std::lock_guard<std::mutex> lock(tracks_mutex_);
    send_tracks_.erase(media_ssrc);
}

void RtcpDispatcher::SetTransportFeedbackCallback(TransportFeedbackCallback cb)
{
    std::lock_guard<std::mutex> lock(tracks_mutex_);
    transport_feedback_cb_ = std::move(cb);
}

void RtcpDispatcher::OnSenderReport(uint32_t sender_ssrc,
                                    uint64_t ntp,
                                    uint32_t rtp_ts,
                                    uint32_t packet_count,
                                    uint32_t octet_count)
{
    std::shared_ptr<RtpReceiverTrack> track;
    {
        std::lock_guard<std::mutex> lock(tracks_mutex_);
        auto it = recv_tracks_.find(sender_ssrc);
        if (it == recv_tracks_.end())
        {
            return;
        }
        track = it->second.lock();
        if (!track)
        {
            recv_tracks_.erase(it);
            return;
        }
    }
    track->OnRtcpSenderReport(sender_ssrc, ntp, rtp_ts, packet_count, octet_count);
}

void RtcpDispatcher::OnNack(uint32_t sender_ssrc, uint32_t media_ssrc, const uint16_t* seqs, size_t count)
{
    (void)sender_ssrc;
    if(!seqs || count == 0)
    {
        return;
    }

    std::shared_ptr<RtpSenderTrack> track;
    {
        std::lock_guard<std::mutex> lock(tracks_mutex_);
        auto it = send_tracks_.find(media_ssrc);
        if (it == send_tracks_.end())
        {
            return;
        }
        track = it->second.lock();
        if (!track)
        {
            send_tracks_.erase(it);
            return;
        }
    }
    std::vector<uint16_t> lost_seqs(seqs, seqs + count);
    track->OnRtcpNack(lost_seqs);
}

void RtcpDispatcher::OnReceiverReport(uint32_t reporter_ssrc, uint32_t media_ssrc, uint8_t fraction_lost, int32_t cumulative_lost, uint32_t highest_seq, uint32_t jitter, uint32_t lsr, uint32_t dlsr)
{
    std::shared_ptr<RtpSenderTrack> track;
    {
        std::lock_guard<std::mutex> lock(tracks_mutex_);
        auto it = send_tracks_.find(media_ssrc);
        if (it == send_tracks_.end())
        {
            LOG_INFO("[RTCP][RR] sender track not found", " media_ssrc=", media_ssrc);
            return;
        }
        track = it->second.lock();
        if (!track)
        {
            send_tracks_.erase(it);
            LOG_INFO("[RTCP][RR] sender track expired", " media_ssrc=", media_ssrc);
            return;
        }
    }
    track->OnRtcpReceiverReport(reporter_ssrc,
                                media_ssrc,
                                fraction_lost,
                                cumulative_lost,
                                highest_seq,
                                jitter,
                                lsr,
                                dlsr);
}

void RtcpDispatcher::OnTransportFeedback(const rtcpx::TransportFeedbackReport& report)
{
    TransportFeedbackCallback cb;
    {
        std::lock_guard<std::mutex> lock(tracks_mutex_);
        cb = transport_feedback_cb_;
    }
    if (cb)
    {
        cb(report);
    }
}
void RtcpDispatcher::OnPli(uint32_t sender_ssrc, uint32_t media_ssrc)
{
    (void)sender_ssrc;
    std::shared_ptr<RtpSenderTrack> track;
    {
        std::lock_guard<std::mutex> lock(tracks_mutex_);
        auto it = send_tracks_.find(media_ssrc);
        if (it == send_tracks_.end())
        {
            return;
        }
        track = it->second.lock();
        if (!track)
        {
            send_tracks_.erase(it);
            return;
        }
    }
    track->OnRtcpPli();
}

void RtcpDispatcher::OnFir(uint32_t sender_ssrc, uint32_t media_ssrc, uint8_t seq_nr)
{
    (void)sender_ssrc;
    (void)seq_nr;
    std::shared_ptr<RtpSenderTrack> track;
    {
        std::lock_guard<std::mutex> lock(tracks_mutex_);
        auto it = send_tracks_.find(media_ssrc);
        if (it == send_tracks_.end())
        {
            return;
        }
        track = it->second.lock();
        if (!track)
        {
            send_tracks_.erase(it);
            return;
        }
    }
    track->OnRtcpFir();
}

void RtcpDispatcher::OnBye(uint32_t sender_ssrc)
{
    std::shared_ptr<RtpReceiverTrack> track;
    {
        std::lock_guard<std::mutex> lock(tracks_mutex_);
        auto it = recv_tracks_.find(sender_ssrc);
        if (it == recv_tracks_.end())
        {
            return;
        }
        track = it->second.lock();
        recv_tracks_.erase(it);
    }
    if (track)
    {
        track->OnRtcpBye(sender_ssrc);
    }
}

void RtcpDispatcher::OnRttUpdated(uint32_t media_ssrc, uint32_t rtt_ms)
{
    (void)media_ssrc;
    (void)rtt_ms;
}

}
