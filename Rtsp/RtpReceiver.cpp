#include "RtpReceiver.h"
#include "logger.h"

#include <algorithm>
#include <chrono>

namespace rtsp 
{

namespace
{
const uint8_t kAnnexBStartCode[] = {0, 0, 0, 1};

void AppendAnnexBNalu(std::vector<uint8_t>& out, const std::vector<uint8_t>& nalu)
{
    if (nalu.empty())
    {
        return;
    }
    out.insert(out.end(), kAnnexBStartCode,
               kAnnexBStartCode + sizeof(kAnnexBStartCode));
    out.insert(out.end(), nalu.begin(), nalu.end());
}

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

RtpReceiverTrack::Ptr RtpReceiverTrack::Create(const TrackInfo& info)
{
    switch (info.type)
    {
    case TrackVideo:
        return info.codec_id == CodecId::H264
                   ? std::make_shared<RtpVideoTracker>(info)
                   : nullptr;
    case TrackAudio:
        switch (info.codec_id)
        {
        case CodecId::PCMU:
        case CodecId::PCMA:
        case CodecId::OPUS:
        case CodecId::AAC:
            return std::make_shared<RtpAudioTracker>(info);
        default:
            return nullptr;
        }
    default:
        return nullptr;
    }
}


RtpPacket::Ptr RtpVideoTracker::inputRtp(uint8_t *ptr, size_t len)
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

    if ((_info.payload_type != 0xFF && hdr.getPayloadType() != _info.payload_type) ||
        (_info.ssrc != 0 && hdr.getSSRC() != _info.ssrc))
    {
        LOG_ERROR("inputRtp packet does not belong to receiver track, expected_pt=",
                  static_cast<int>(_info.payload_type), " actual_pt=",
                  static_cast<int>(hdr.getPayloadType()), " expected_ssrc=",
                  _info.ssrc, " actual_ssrc=", hdr.getSSRC());
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
    pkt->setTrackType(_info.type);
    pkt->setSampleRate(_info.clock_rate > 0 ? _info.clock_rate : 90000U);
    pkt->setTrackIndex(_info.track_index);
    pkt->setRaw(ptr, len);
    pkt->setHeaderInfo(headerLen, headerLen, payloadLen);
    pkt->setRecvTimeMs(NowMs());

    if (_nack_receiver)
    {
        _nack_receiver->OnReceivedPacket(pkt->getSeq(), pkt->getRecvTimeMs());
        // Keep this immediate pass for low first-NACK latency. TickNack() also
        // drives retries when a periodic media tick is available.
        _nack_receiver->Process(pkt->getRecvTimeMs());
    }

    if (!inputUnorderedPacket(pkt))
    {
        return nullptr;
    }
    return pkt;
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
        frame->info.timestamp.receive_time_ms = pkt->getRecvTimeMs();
        frame->info.integrity = au.broken
                                    ? media::FrameIntegrity::Corrupted
                                    : (au.complete ? media::FrameIntegrity::Complete
                                                   : media::FrameIntegrity::Incomplete);

        frame->rtp.ssrc              = au.ssrc;
        frame->rtp.rtp_timestamp     = au.timestamp;
        frame->rtp.first_sequence    = au.first_seq;
        frame->rtp.last_sequence     = au.last_seq;
        frame->rtp.packet_count      = static_cast<uint16_t>(au.last_seq - au.first_seq) + 1;

        frame->video.is_idr          = au.has_idr;
        frame->video.has_sps         = au.has_sps;
        frame->video.has_pps         = au.has_pps;
        if (const auto* sps = _depacketizer->parameterSets().LatestSps())
        {
            frame->video.width = sps->width;
            frame->video.height = sps->height;
        }

        const bool config_only = !au.has_idr && (au.has_sps || au.has_pps) &&
            std::all_of(au.nalus.begin(), au.nalus.end(), [](const std::vector<uint8_t>& nalu) {
                if (nalu.empty()) return false;
                const uint8_t type = media::H264GetNalType(nalu[0]);
                return type == static_cast<uint8_t>(media::H264NalType::Sps) ||
                       type == static_cast<uint8_t>(media::H264NalType::Pps);
            });
        frame->frame_type = config_only
                                ? media::EncodedFrameType::Config
                                : (au.has_idr ? media::EncodedFrameType::Key
                                              : media::EncodedFrameType::Delta);
        frame->sample_rate           = pkt->getSampleRate();

        auto buffer = std::make_shared<std::vector<uint8_t>>();
        size_t annexb_size = 0;
        for (const auto& nalu : au.nalus)
            annexb_size += sizeof(kAnnexBStartCode) + nalu.size();
        if (au.has_idr && !au.has_sps)
        {
            if (const auto* sps = _depacketizer->parameterSets().LatestSps())
                annexb_size += sizeof(kAnnexBStartCode) + sps->payload.size();
        }
        if (au.has_idr && !au.has_pps)
        {
            if (const auto* pps = _depacketizer->parameterSets().LatestPps())
                annexb_size += sizeof(kAnnexBStartCode) + pps->payload.size();
        }
        buffer->reserve(annexb_size);
        if (au.has_idr)
        {
            if (!au.has_sps)
            {
                if (const auto* sps = _depacketizer->parameterSets().LatestSps())
                {
                    AppendAnnexBNalu(*buffer, sps->payload);
                    frame->video.has_sps = true;
                    frame->video.parameter_sets_injected = true;
                }
            }
            if (!au.has_pps)
            {
                if (const auto* pps = _depacketizer->parameterSets().LatestPps())
                {
                    AppendAnnexBNalu(*buffer, pps->payload);
                    frame->video.has_pps = true;
                    frame->video.parameter_sets_injected = true;
                }
            }
        }
        for (const auto& nalu : au.nalus)
        {
            AppendAnnexBNalu(*buffer, nalu);
        }
        frame->buffer                = std::move(buffer);
        frame->size                  = frame->buffer->size();

        emitEncodedFrame(frame);
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
    uint16_t channels     = info.channels > 0 ? static_cast<uint16_t>(info.channels) : 1;
    depacketizer_ = media::AudioRtpDepacketizerFactory::Create(
        codec, info.codec_name, sample_rate, channels, info.fmtp);
    if (!depacketizer_)
    {
        LOG_ERROR("[RtpAudioTracker] failed to create audio RTP depacketizer",
                  " codec=", info.codec_name,
                  " pt=", static_cast<int>(info.payload_type));
    }
    else
    {
        LOG_INFO("[AUDIO][TRACK] depacketizer ready",
                 " track=", info.track_index,
                 " codec=", info.codec_name,
                 " pt=", static_cast<int>(info.payload_type),
                 " clock_rate=", sample_rate,
                 " channels=", channels);
    }
}

RtpAudioTracker::~RtpAudioTracker()
{

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

RtpPacket::Ptr RtpAudioTracker::inputRtp(uint8_t* ptr, size_t len)
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

    if ((_info.payload_type != 0xFF && hdr.getPayloadType() != _info.payload_type) ||
        (_info.ssrc != 0 && hdr.getSSRC() != _info.ssrc))
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

    pkt->setTrackType(_info.type);
    pkt->setSampleRate(_info.clock_rate);
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
        clone->inputRtp(ptr, len);
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

    ++audio_packets_seen_;
    if (audio_packets_seen_ == 1)
    {
        LOG_INFO("[AUDIO][RTP] first packet accepted",
                 " track=", _info.track_index,
                 " codec=", _info.codec_name,
                 " pt=", static_cast<int>(pkt->getPayloadType()),
                 " ssrc=", pkt->getSSRC(),
                 " seq=", pkt->getSeq(),
                 " rtp_ts=", pkt->getStamp(),
                 " payload_bytes=", payload_size);
    }

    if (!depacketizer_->Input(view))
    {
        return;
    }

    media::EncodedFrame completed;
    while (depacketizer_->PopFrame(completed))
    {
        ++audio_frames_completed_;
        completed.info.track_id = _info.track_index >= 0
                                      ? static_cast<media::TrackId>(_info.track_index)
                                      : 0;
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

void RtcpDispatcher::SetSenderReportCallback(SenderReportCallback cb)
{
    std::lock_guard<std::mutex> lock(tracks_mutex_);
    sender_report_cb_ = std::move(cb);
}

void RtcpDispatcher::OnSenderReport(uint32_t sender_ssrc, uint64_t ntp, uint32_t rtp_ts, uint32_t packet_count, uint32_t octet_count)
{
    std::shared_ptr<RtpReceiverTrack> track;
    SenderReportCallback callback;
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
        callback = sender_report_cb_;
    }
    track->OnRtcpSenderReport(sender_ssrc, ntp, rtp_ts, packet_count, octet_count);
    if (callback)
    {
        callback(sender_ssrc);
    }
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

/* Find the sender track by media SSRC and dispatch the RTCP Receiver Report to it. */
void RtcpDispatcher::OnReceiverReport(uint32_t reporter_ssrc, uint32_t media_ssrc, uint8_t fraction_lost, int32_t cumulative_lost, uint32_t highest_seq, uint32_t jitter, uint32_t lsr, uint32_t dlsr)
{
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
    track->OnRtcpReceiverReport(reporter_ssrc, media_ssrc, fraction_lost, cumulative_lost, highest_seq, jitter, lsr, dlsr);
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
    std::shared_ptr<RtpReceiverTrack> track;
    {
        std::lock_guard<std::mutex> lock(tracks_mutex_);
        auto it = recv_tracks_.find(media_ssrc);
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

    track->SetRttMs(rtt_ms);
    if (auto video = std::dynamic_pointer_cast<RtpVideoTracker>(track))
    {
        video->UpdateNackRtt(rtt_ms);
    }
}

}
