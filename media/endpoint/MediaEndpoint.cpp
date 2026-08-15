#include "MediaEndpoint.h"
#include "logger.h"
#include "RtcpContext.h"
#include "RtcpReciver.h"
#include "RtcpHealper.h"
#include "MediaStreamAffinity.h"
#include "RtcpNack.h"
#include "RtcpFeedback.h"
#include <iomanip>
#include <mutex>
#include <algorithm>
#include <chrono>
#include <limits>
#include "utils.h"

namespace media 
{

namespace
{
uint64_t SteadyNowMs()
{
    using namespace std::chrono;
    return duration_cast<milliseconds>(steady_clock::now().time_since_epoch()).count();
}

uint32_t ClampToUint32(double value)
{
    if (value <= 0.0)
    {
        return 0;
    }
    const double maximum = static_cast<double>(std::numeric_limits<uint32_t>::max());
    return static_cast<uint32_t>(std::min(value, maximum));
}
}

static void DumpBytes(const uint8_t* data, size_t len, size_t max_dump = 16)
{
    if (!data)
        return;

    std::ostringstream oss;
    size_t n = std::min(len, max_dump);
    for (size_t i = 0; i < n; ++i)
    {
        oss << std::hex << std::setw(2) << std::setfill('0') << static_cast<int>(data[i]) << " ";
    }
    LOG_INFO("packet dump len=", len, " bytes=", oss.str());
}

static uint16_t ReadUint16BE(const uint8_t* data)
{
    return (static_cast<uint16_t>(data[0]) << 8) | static_cast<uint16_t>(data[1]);
}

static uint32_t ReadUint32BE(const uint8_t* data)
{
    return (static_cast<uint32_t>(data[0]) << 24) |
           (static_cast<uint32_t>(data[1]) << 16) |
           (static_cast<uint32_t>(data[2]) << 8) |
           static_cast<uint32_t>(data[3]);
}

/* 网络协议包 转成 RtpPacket */
void MediaEndpoint::OnRtp(WorkJob& job)
{
    if (!job.raw.data || job.raw.len == 0)
    {
        LOG_ERROR("invalid raw rtp packet");
        return;
    }
    Packet pkt;
    pkt.assign(job.raw.data, job.raw.len);
    pkt.enqueue_ts = job.enqueue_ts;
    HandleRtpPacket(&pkt);
}

void MediaEndpoint::OnRtcp(WorkJob& job)
{
    if (!job.raw.data || job.raw.len == 0)
    {
        LOG_ERROR("invalid raw rtcp packet");
        return;
    }

    Packet pkt;
    pkt.assign(job.raw.data, job.raw.len);
    pkt.enqueue_ts = job.enqueue_ts;
    HandleRtcpPacket(&pkt);
}

void MediaEndpoint::OnStun(WorkJob& job)
{
    if (!job.raw.data || job.raw.len == 0)
    {
        return;
    }

    Packet pkt;
    pkt.assign(job.raw.data, job.raw.len);
    pkt.enqueue_ts = job.enqueue_ts;
    HandleStunPacket(&pkt);
}

void MediaEndpoint::OnDtls(WorkJob& job)
{
    if (!job.raw.data || job.raw.len == 0)
    {
        return;
    }

    Packet pkt;
    pkt.assign(job.raw.data, job.raw.len);
    pkt.enqueue_ts = job.enqueue_ts;
    HandleDtlsPacket(&pkt);
}


rtsp::RtpReceiverTrack::Ptr SfuEndpoint::FindTrackBySsrc(uint32_t ssrc)
{
    std::lock_guard<std::mutex> lock(track_mtx_);

    auto it = ssrc_to_track_.find(ssrc);
    if (it == ssrc_to_track_.end())
    {
        return nullptr;
    }

    return it->second;
}

void SfuEndpoint::HandleRtpPacket(Packet* pkt)
{
    if (!pkt || pkt->len < 12)
    {
        return;
    }

    const uint8_t* data = pkt->data;

    uint8_t vpxcc = data[0];
    uint8_t mpt   = data[1];

    uint8_t version      = (vpxcc >> 6) & 0x03;
    uint8_t padding      = (vpxcc >> 5) & 0x01;
    uint8_t extension    = (vpxcc >> 4) & 0x01;
    uint8_t csrc_count   = vpxcc & 0x0F;

    uint8_t marker       = (mpt >> 7) & 0x01;
    uint8_t payload_type = mpt & 0x7F;

    uint16_t seq = (data[2] << 8) | data[3];

    uint32_t timestamp = (uint32_t(data[4]) << 24) | (uint32_t(data[5]) << 16) | (uint32_t(data[6]) << 8)  | data[7];

    uint32_t ssrc = (uint32_t(data[8]) << 24) | (uint32_t(data[9]) << 16) | (uint32_t(data[10]) << 8) | data[11];

    media_affinity::TryGetRtpSsrc(pkt->data, pkt->len, ssrc);

    auto source_track = SourceTrack();
    auto source_session = SourceSession();
    if (source_track)
    {
        const auto& info = source_track->getTrackInfo();
        if (info.payload_type != 0xFF && payload_type != info.payload_type)
        {
            return;
        }

        if (source_session)
        {
            PayloadTypeInfo pt_info;
            if (source_session->FindPayloadType(info.track_index, payload_type, &pt_info))
            {
                const bool track_type_mismatch =
                    (info.type == TrackVideo && pt_info.track_type != StreamTrackType::Video) ||
                    (info.type == TrackAudio && pt_info.track_type != StreamTrackType::Audio);
                if (track_type_mismatch)
                {
                    LOG_ERROR("[RTP] SDP payload type track mismatch",
                              " pt=", static_cast<int>(payload_type),
                              " endpoint_track=", TrackTypeToString(info.type),
                              " sdp_codec=", pt_info.codec_name,
                              " ssrc=", ssrc);
                    return;
                }
            }
        }
    }

    auto track = GetOrCreateTrack(ssrc);
    if (!track)
    {
        return;
    }

    const auto& track_info = track->getTrackInfo();
    const int sample_rate = track_info.clock_rate > 0 ? static_cast<int>(track_info.clock_rate) : 90000;
    if (track_info.type == TrackAudio)
    {
        LOG_DEBUG("[AUDIO][RTP] received",
                  " track=", track_info.track_index,
                  " codec=", track_info.codec_name,
                  " pt=", static_cast<int>(payload_type),
                  " ssrc=", ssrc,
                  " seq=", seq,
                  " rtp_ts=", timestamp,
                  " bytes=", pkt->len);
    }
    if (track->inputRtp(track->getTrackType(), sample_rate, pkt->data, pkt->len))
    {
        ForwardRtpToSubscribers(ssrc, pkt->data, pkt->len);
    }
}

std::shared_ptr<rtsp::RtpReceiverTrack> SfuEndpoint::GetOrCreateTrack(uint32_t ssrc)
{
    {
        std::lock_guard<std::mutex> lock(track_mtx_);
        auto it = ssrc_to_track_.find(ssrc);
        if (it != ssrc_to_track_.end())
        {
            LOG_DEBUG("[TRACK] found existing track", "ssrc=", ssrc, "track_ptr=", it->second.get());
            return it->second;
        }
    }

    std::shared_ptr<rtsp::RtpReceiverTrack> new_track;
    auto source_track = SourceTrack();
    if (!source_track)
    {
        LOG_ERROR("[TRACK] source track not found, ssrc=", ssrc);
        return nullptr;
    }

    TrackInfo info = source_track->getTrackInfo();
    info.ssrc = ssrc;

    if (info.type == TrackVideo)
    {
        new_track = std::make_shared<rtsp::RtpVideoTracker>(info);
    }
    else if (info.type == TrackAudio)
    {
        new_track = std::make_shared<rtsp::RtpAudioTracker>(info);
        LOG_INFO("[AUDIO][TRACK] created",
                 " track=", info.track_index,
                 " codec=", info.codec_name,
                 " pt=", static_cast<int>(info.payload_type),
                 " ssrc=", ssrc,
                 " clock_rate=", info.clock_rate,
                 " channels=", info.channels);
    }
    else
    {
        LOG_ERROR("[TRACK] unsupported track type", " ssrc=", ssrc, " type=", static_cast<int>(info.type));
        return nullptr;
    }

    std::weak_ptr<SfuEndpoint> Weak_self = weak_from_this();

    new_track->setSendNackCallback(
    [Weak_self](uint32_t media_ssrc, const std::vector<uint16_t>& lost_seqs) {
        auto self = Weak_self.lock();
        if (!self || lost_seqs.empty()) 
        {
            return;
        }

        self->OnTrackNack(media_ssrc, lost_seqs);
    });

    new_track->setOnEncodedFrame(
        [Weak_self](const rtsp::RtpReceiverTrack::EncodedFramePtr& frame) {
            if (auto self = Weak_self.lock())
            {
                self->DispatchEncodedFrame(frame);
            }
        });

    {
        std::lock_guard<std::mutex> lock(track_mtx_);
        auto result = ssrc_to_track_.emplace(ssrc, new_track);
        if (!result.second)
        {
            return result.first->second;
        }
    }

    if (rtcp_dispatcher_)
    {
        rtcp_dispatcher_->AddReceiverTrack(ssrc, new_track);
    }

    return new_track;
}

void SfuEndpoint::HandleRtcpPacket(Packet* pkt)
{
    if (!pkt || pkt->len < 4)
    {
        LOG_ERROR("invalid rtcp packet");
        return;
    }

    const uint8_t first_byte    = pkt->data[0];
    const uint8_t version       = (first_byte >> 6) & 0x03;
    const uint8_t count_or_fmt  = first_byte & 0x1F;
    const uint8_t packet_type   = pkt->data[1];
    const uint16_t length_words = utils::Utils::ReadUint16BE(pkt->data + 2);

    rtcpx::RtcpPacketInfo rtcp_info;
    if (!rtcpx::InspectRtcpPacket(pkt->data, pkt->len, &rtcp_info))
    {
        LOG_ERROR("[RTCP] invalid rtcp packet, len=", pkt->len);
        return;
    }

    if (rtcp_info.has_media_ssrc &&
        (rtcp_info.first_packet_type == rtcpx::RTCP_PT_SR ||
         rtcp_info.first_packet_type == rtcpx::RTCP_PT_SDES ||
         rtcp_info.first_packet_type == rtcpx::RTCP_PT_BYE))
    {
        GetOrCreateTrack(rtcp_info.media_ssrc);
    }

    if (!rtcp_receiver_ || !rtcp_receiver_->OnRtcpPacket(pkt->data, pkt->len))
    {
        LOG_ERROR("[RTCP] parse failed, len=", pkt->len);
    }
}

bool SfuEndpoint::InitTracks(const std::vector<TrackInfo>& infos)
{
    for (const auto& info : infos)
    {
        if (info.type == TrackType::TrackVideo)
        {
            video_track_ = std::make_shared<VideoTrack>(info);
        }
    }
    return video_track_ != nullptr;
}


bool SfuEndpoint::Start()
{
    if (!rtcp_dispatcher_)
    {
        rtcp_dispatcher_ = std::make_unique<rtsp::RtcpDispatcher>();
    }
    if (!rtcp_receiver_)
    {
        rtcp_receiver_ = std::make_unique<rtcpx::RtcpReceiverImpl>(rtcp_dispatcher_.get());
    }
    std::weak_ptr<SfuEndpoint> weak_self = weak_from_this();
    rtcp_dispatcher_->SetSenderReportCallback(
        [weak_self](uint32_t media_ssrc) {
            if (auto self = weak_self.lock())
            {
                self->EvaluateReceiveQuality(media_ssrc);
            }
        });
    local_rtcp_ssrc_ = static_cast<uint32_t>(Id() ^ (Id() >> 32));
    if (local_rtcp_ssrc_ == 0)
    {
        local_rtcp_ssrc_ = 1;
    }
    rtcp_receiver_->SetLocalSsrc(local_rtcp_ssrc_);
    SetState(State::kRunning);
    return true;
}

void SfuEndpoint::SetRtcpSendCallback(SendRtcpCallback cb)
{
    std::lock_guard<std::mutex> lock(rtcp_send_mutex_);
    send_rtcp_cb_ = std::move(cb);
}

SfuEndpoint::FrameSubscriptionId SfuEndpoint::AddEncodedFrameCallback(EncodedFrameCallback cb)
{
    if (!cb)
    {
        return 0;
    }

    const FrameSubscriptionId id = next_frame_subscription_id_.fetch_add(1);
    std::lock_guard<std::mutex> lock(frame_callbacks_mutex_);
    frame_callbacks_[id] = std::move(cb);
    return id;
}

void SfuEndpoint::RemoveEncodedFrameCallback(FrameSubscriptionId id)
{
    if (id == 0)
    {
        return;
    }
    std::lock_guard<std::mutex> lock(frame_callbacks_mutex_);
    frame_callbacks_.erase(id);
}

void SfuEndpoint::DispatchEncodedFrame(const media::EncodedFrame::Ptr& frame)
{
    if (!frame || !frame->Valid())
    {
        return;
    }

    std::vector<EncodedFrameCallback> callbacks;
    {
        std::lock_guard<std::mutex> lock(frame_callbacks_mutex_);
        callbacks.reserve(frame_callbacks_.size());
        for (const auto& entry : frame_callbacks_)
        {
            callbacks.push_back(entry.second);
        }
    }

    const media::EncodedFrame::ConstPtr immutable_frame = frame;
    if (frame_publisher_)
    {
        EncodedFrameEvent event;
        event.source.endpoint_id = Id();
        event.source.track_id = frame->info.track_id;
        event.source.ssrc = frame->rtp.ssrc;
        if (const auto session = SourceSession())
        {
            event.source.session_id = std::to_string(session->GetId());
            event.source.stream_id = session->GetRtspSuffix();
        }
        event.frame = immutable_frame;
        const size_t accepted = frame_publisher_->Publish(event);
        const uint64_t count = published_frame_count_.fetch_add(1) + 1;
        if (count == 1 || count % 300 == 0)
        {
            LOG_INFO("[FRAME_SOURCE] encoded frame published, count=", count,
                     ", endpoint_id=", event.source.endpoint_id,
                     ", session_id=", event.source.session_id,
                     ", stream_id=", event.source.stream_id,
                     ", track_id=", event.source.track_id,
                     ", ssrc=", event.source.ssrc,
                     ", media_type=", static_cast<int>(frame->info.media_type),
                     ", codec=", static_cast<int>(frame->info.codec),
                     ", bytes=", frame->size,
                     ", accepted_sinks=", accepted);
        }
    }
    for (const auto& callback : callbacks)
    {
        callback(immutable_frame);
    }
}

void SfuEndpoint::OnTrackNack(uint32_t media_ssrc, const std::vector<uint16_t>& lost_seqs)
{
    if (lost_seqs.empty())
    {
        return;
    }

    SendRtcpCallback send;
    {
        std::lock_guard<std::mutex> lock(rtcp_send_mutex_);
        send = send_rtcp_cb_;
    }
    if (!send)
    {
        return;
    }

    const auto packet = rtcpx::RtRtcpNack::Build(local_rtcp_ssrc_, media_ssrc, lost_seqs);
    if (packet.empty() || !send(packet.data(), packet.size()))
    {
        LOG_ERROR("[RTCP][NACK] send failed", " media_ssrc=", media_ssrc, " count=", lost_seqs.size());
    }
}

void SfuEndpoint::OnTrackPli(uint32_t media_ssrc)
{
    SendRtcpCallback send;
    {
        std::lock_guard<std::mutex> lock(rtcp_send_mutex_);
        send = send_rtcp_cb_;
    }
    if (!send)
    {
        return;
    }

    const auto packet = rtcpx::RtcpFeedback::BuildPli(local_rtcp_ssrc_, media_ssrc);
    if (!send(packet.data(), packet.size()))
    {
        LOG_ERROR("[RTCP][PLI] send failed", " media_ssrc=", media_ssrc);
    }
}

void SfuEndpoint::Stop()
{
    SetState(State::kStopping);
    if (rtcp_dispatcher_)
    {
        rtcp_dispatcher_->SetSenderReportCallback({});
    }

    std::vector<uint32_t> streams;
    {
        std::lock_guard<std::mutex> lock(track_mtx_);
        streams.reserve(ssrc_to_track_.size());
        for (const auto& item : ssrc_to_track_)
        {
            streams.push_back(item.first);
        }
    }

    if (streams.empty())
    {
        SetState(State::kStopped);
        return;
    }

    auto remaining = std::make_shared<std::atomic<size_t>>(streams.size());
    std::weak_ptr<SfuEndpoint> weak_self = weak_from_this();
    for (uint32_t ssrc : streams)
    {
        const int ret = media_affinity::PostToMediaStream(
            Id(), ssrc, [weak_self, remaining, ssrc] {
                if (auto self = weak_self.lock())
                {
                    self->RemoveMediaStreamOnOwner(ssrc);
                    if (remaining->fetch_sub(1) == 1)
                    {
                        self->SetState(State::kStopped);
                    }
                }
            });
        if (ret != 0 && remaining->fetch_sub(1) == 1)
        {
            SetState(State::kStopped);
        }
    }
}

void SfuRouter::AddSubscriber(uint32_t source_ssrc, std::shared_ptr<rtsp::RtpSenderTrack> sender)
{
    std::lock_guard<std::mutex> lock(mutex_);
    subscribers_[source_ssrc].push_back(sender);
}

void SfuRouter::RemoveSubscriber(uint32_t source_ssrc, const rtsp::RtpSenderTrack* sender)
{
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = subscribers_.find(source_ssrc);
    if (it == subscribers_.end())
    {
        return;
    }
    auto& tracks = it->second;
    tracks.erase(std::remove_if(tracks.begin(), tracks.end(),
                                [sender](const auto& item) {
                                    return !item || item.get() == sender;
                                }),
                 tracks.end());
    if (tracks.empty())
    {
        subscribers_.erase(it);
    }
}

void SfuRouter::RemoveStream(uint32_t source_ssrc)
{
    std::lock_guard<std::mutex> lock(mutex_);
    subscribers_.erase(source_ssrc);
}

std::vector<std::shared_ptr<rtsp::RtpSenderTrack>> SfuRouter::GetSenderTracks(uint32_t source_ssrc)
{
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = subscribers_.find(source_ssrc);
    if (it == subscribers_.end())
    {
        return {};
    }

    return it->second;
}

int SfuEndpoint::AddSubscriber(uint32_t source_ssrc, std::shared_ptr<rtsp::RtpSenderTrack> sender)
{
    if (!sender)
    {
        return -1;
    }
    std::weak_ptr<SfuEndpoint> weak_self = weak_from_this();
    return media_affinity::PostToMediaStream(
        Id(), source_ssrc,
        [weak_self, source_ssrc, sender = std::move(sender)] {
            if (auto self = weak_self.lock())
            {
                std::weak_ptr<SfuEndpoint> sender_weak_self = self;
                sender->SetKeyFrameRequestCallback([sender_weak_self, source_ssrc] {
                    if (auto endpoint = sender_weak_self.lock())
                    {
                        endpoint->OnTrackPli(source_ssrc);
                    }
                });
                if (self->rtcp_dispatcher_)
                {
                    self->rtcp_dispatcher_->AddSenderTrack(sender->GetSsrc(), sender);
                }
                self->router_.AddSubscriber(source_ssrc, sender);
            }
        });
}

int SfuEndpoint::RemoveSubscriber(uint32_t source_ssrc, const std::shared_ptr<rtsp::RtpSenderTrack>& sender)
{
    if (!sender)
    {
        return -1;
    }
    std::weak_ptr<SfuEndpoint> weak_self = weak_from_this();
    const auto* sender_ptr = sender.get();
    const uint32_t sender_ssrc = sender->GetSsrc();
    return media_affinity::PostToMediaStream(
        Id(), source_ssrc, [weak_self, source_ssrc, sender_ptr, sender_ssrc] {
            if (auto self = weak_self.lock())
            {
                self->router_.RemoveSubscriber(source_ssrc, sender_ptr);
                if (self->rtcp_dispatcher_)
                {
                    self->rtcp_dispatcher_->RemoveSenderTrack(sender_ssrc);
                }
            }
        });
}

int SfuEndpoint::RemoveMediaStream(uint32_t source_ssrc)
{
    std::weak_ptr<SfuEndpoint> weak_self = weak_from_this();
    return media_affinity::PostToMediaStream(
        Id(), source_ssrc, [weak_self, source_ssrc] {
            if (auto self = weak_self.lock())
            {
                self->RemoveMediaStreamOnOwner(source_ssrc);
            }
        });
}

void SfuEndpoint::RemoveMediaStreamOnOwner(uint32_t source_ssrc)
{
    {
        std::lock_guard<std::mutex> lock(track_mtx_);
        ssrc_to_track_.erase(source_ssrc);
    }
    if (rtcp_dispatcher_)
    {
        rtcp_dispatcher_->RemoveReceiverTrack(source_ssrc);
        for (const auto& sender : router_.GetSenderTracks(source_ssrc))
        {
            if (sender)
            {
                rtcp_dispatcher_->RemoveSenderTrack(sender->GetSsrc());
            }
        }
    }
    router_.RemoveStream(source_ssrc);
    {
        std::lock_guard<std::mutex> lock(quality_mutex_);
        receive_quality_.erase(source_ssrc);
    }
}

void SfuEndpoint::EvaluateReceiveQuality(uint32_t source_ssrc)
{
    auto track = FindTrackBySsrc(source_ssrc);
    if (!track)
    {
        return;
    }

    const uint64_t now_ms = SteadyNowMs();
    const auto report = track->BuildReceiverReport(now_ms);

    WeakNetFeedback feedback;
    feedback.now_ms = now_ms;
    feedback.send_bitrate_bps = ClampToUint32(track->GetSenderBitrateBps());
    feedback.loss_rate = static_cast<double>(report.fraction_lost) / 256.0;
    const uint32_t clock_rate = track->getTrackInfo().clock_rate;
    if (clock_rate > 0)
    {
        feedback.jitter_ms = static_cast<uint32_t>(static_cast<uint64_t>(report.jitter) * 1000ULL / clock_rate);
    }
    feedback.rtt_ms = track->GetRttMs();
    feedback.nack_count = track->GetNackCount();
    feedback.pli_count = track->GetPliCount();
    feedback.fir_count = track->GetFirCount();

    NetworkControlUpdate update;
    NetworkQualityLevel previous_quality = NetworkQualityLevel::Unknown;
    NetworkQualityLevel current_quality = NetworkQualityLevel::Unknown;
    {
        std::lock_guard<std::mutex> lock(quality_mutex_);
        auto& state = receive_quality_[source_ssrc];
        previous_quality = state.quality;
        update = state.controller.OnFeedback(feedback);
        state.latest_report = report;
        state.update_time_ms = now_ms;
        current_quality = state.controller.GetSnapshot().quality;
        state.quality = current_quality;
    }

    if (current_quality != previous_quality)
    {
        LOG_INFO("[WEAK_NET] quality changed",
                 " ssrc=", source_ssrc,
                 " from=", ToString(previous_quality),
                 " to=", ToString(current_quality),
                 " loss=", feedback.loss_rate,
                 " jitter_ms=", feedback.jitter_ms,
                 " sender_bitrate_bps=", feedback.send_bitrate_bps,
                 " target_bitrate_bps=", update.target_rate.target_bitrate_bps);
    }

    if (update.request_key_frame && track->getTrackType() == TrackVideo)
    {
        OnTrackPli(source_ssrc);
    }
}

void SfuEndpoint::ForwardRtpToSubscribers(uint32_t source_ssrc, const uint8_t* data, size_t len)
{
    auto senders = router_.GetSenderTracks(source_ssrc);

    if (!senders.empty())
    {
        auto source = FindTrackBySsrc(source_ssrc);
        if (source && source->getTrackType() == TrackAudio)
        {
            LOG_DEBUG("[AUDIO][RTP] forwarding",
                      " ssrc=", source_ssrc,
                      " subscribers=", senders.size(),
                      " bytes=", len);
        }
    }

    for (auto& sender : senders)
    {
        if (!sender)
        {
            continue;
        }

        sender->InputRtpPacket(data, len);
    }
}

}
