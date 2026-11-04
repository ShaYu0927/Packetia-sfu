#include "SfuEndpoint.h"
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
#include "utils.h"

namespace media 
{

SfuEndpoint::SfuEndpoint(uint64_t id, std::shared_ptr<RtpTrackDescription> track_description,
                         const EncodedFrameSource& source,
                         std::shared_ptr<IEncodedFramePublisher> frame_publisher)
    : utils::EndpointBase(id, "SfuEndpoint"),
      frames_(id, source, std::move(frame_publisher))
{
    media_stream_id_ = source.media_stream_id;
    if (track_description)
        AddPublishedTrack(std::to_string(track_description->getTrackIndex()), track_description);
}

namespace
{
uint64_t SteadyNowMs()
{
    using namespace std::chrono;
    return duration_cast<milliseconds>(steady_clock::now().time_since_epoch()).count();
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

rtsp::RtpReceiverTrack::Ptr SfuEndpoint::FindReceiverTrackBySsrc(uint32_t ssrc)
{
    std::lock_guard<std::mutex> lock(track_mtx_);

    auto it = ssrc_to_track_.find(ssrc);
    if (it == ssrc_to_track_.end())
    {
        return nullptr;
    }

    return it->second;
}

void SfuEndpoint::HandleRtpPacket(common::BufferView packet)
{
    std::lock_guard<std::recursive_mutex> guard(runtime_mutex_);
    if (!IsRunning() || !ResolveRtpTrack(packet, {})) return;
    if (packet.Size() < 12)
    {
        return;
    }

    const uint8_t* data = packet.Data();

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

    media_affinity::TryGetRtpSsrc(packet.Data(), packet.Size(), ssrc);

    auto track = GetOrCreateReceiverTrack(ssrc);
    if (!track)
    {
        return;
    }

    const auto& track_info = track->getTrackInfo();
    if (track_info.type == TrackAudio)
    {
        LOG_DEBUG("[AUDIO][RTP] received",
                  " track=", track_info.track_index,
                  " codec=", track_info.codec_name,
                  " pt=", static_cast<int>(payload_type),
                  " ssrc=", ssrc,
                  " seq=", seq,
                  " rtp_ts=", timestamp,
                  " bytes=", packet.Size());
    }
    if (track->inputRtp(packet.Data(), packet.Size()))
    {
        ForwardRtpToSubscribers(ssrc, packet.Data(), packet.Size());
    }
}

std::shared_ptr<rtsp::RtpReceiverTrack> SfuEndpoint::GetOrCreateReceiverTrack(uint32_t ssrc)
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
    const auto binding = ssrc_bindings_.find(ssrc);
    if (binding == ssrc_bindings_.end()) return nullptr;
    const auto source = published_tracks_.find(binding->second);
    if (source == published_tracks_.end())
    {
        LOG_ERROR("[TRACK] source track not found, ssrc=", ssrc);
        return nullptr;
    }

    ::TrackInfo info = source->second.description->getTrackInfo();
    info.ssrc = ssrc;

    new_track = rtsp::RtpReceiverTrack::Create(info);
    if (!new_track)
    {
        LOG_ERROR("[TRACK] unsupported receiver track",
                  " ssrc=", ssrc,
                  " type=", static_cast<int>(info.type),
                  " codec=", info.codec_name);
        return nullptr;
    }

    if (info.type == TrackAudio)
    {
        LOG_INFO("[AUDIO][TRACK] created",
                 " track=", info.track_index,
                 " codec=", info.codec_name,
                 " pt=", static_cast<int>(info.payload_type),
                 " ssrc=", ssrc,
                 " clock_rate=", info.clock_rate,
                 " channels=", info.channels);
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

    new_track->setNackRecoveryFailureCallback(
    [Weak_self](uint32_t media_ssrc, const std::vector<uint16_t>& abandoned_seqs) {
        auto self = Weak_self.lock();
        if (!self || abandoned_seqs.empty())
        {
            return;
        }

        LOG_INFO("[RTCP][NACK] recovery exhausted",
                 " media_ssrc=", media_ssrc,
                 " count=", abandoned_seqs.size());
        self->OnTrackPli(media_ssrc);
    });

    new_track->setRequestKeyFrameCallback([Weak_self, ssrc] {
        if (auto self = Weak_self.lock()) self->OnTrackPli(ssrc);
    });

    new_track->setOnEncodedFrame(
        [Weak_self](const rtsp::RtpReceiverTrack::EncodedFramePtr& frame) {
            if (auto self = Weak_self.lock())
            {
                self->pending_frames_.push_back(frame);
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

    try
    {
        receive_feedback_.RegisterSource(ssrc);
        if (rtcp_dispatcher_)
            rtcp_dispatcher_->AddReceiverTrack(ssrc, new_track);
        receiver_tick_keys_[media_affinity::MakeStreamKey(Id(), ssrc)] = new_track;
    }
    catch (...)
    {
        RemoveMediaStreamOnOwner(ssrc);
        throw;
    }

    return new_track;
}

void SfuEndpoint::HandleRtcpPacket(common::BufferView packet)
{
    std::lock_guard<std::recursive_mutex> guard(runtime_mutex_);
    if (!IsRunning()) return;
    PruneSubscriptions();
    if (packet.Size() < 4)
    {
        LOG_ERROR("invalid rtcp packet");
        return;
    }

    const uint8_t first_byte    = packet.Data()[0];
    const uint8_t version       = (first_byte >> 6) & 0x03;
    const uint8_t count_or_fmt  = first_byte & 0x1F;
    const uint8_t packet_type   = packet.Data()[1];
    const uint16_t length_words = utils::Utils::ReadUint16BE(packet.Data() + 2);

    rtcpx::RtcpPacketInfo rtcp_info;
    if (!rtcpx::InspectRtcpPacket(packet.Data(), packet.Size(), &rtcp_info))
    {
        LOG_ERROR("[RTCP] invalid rtcp packet, len=", packet.Size());
        return;
    }

    if (rtcp_info.has_media_ssrc &&
        (rtcp_info.first_packet_type == rtcpx::RTCP_PT_SR ||
         rtcp_info.first_packet_type == rtcpx::RTCP_PT_SDES ||
         rtcp_info.first_packet_type == rtcpx::RTCP_PT_BYE))
    {
        GetOrCreateReceiverTrack(rtcp_info.media_ssrc);
    }

    if (!rtcp_receiver_ || !rtcp_receiver_->OnRtcpPacket(packet.Data(), packet.Size()))
    {
        LOG_ERROR("[RTCP] parse failed, len=", packet.Size());
    }
}

bool SfuEndpoint::Start()
{
    std::lock_guard<std::recursive_mutex> guard(runtime_mutex_);
    if (GetState() == State::kStopped || GetState() == State::kStopping) return false;
    if (!rtcp_dispatcher_)
    {
        rtcp_dispatcher_ = std::make_unique<rtsp::RtcpDispatcher>();
    }
    downlinks_.SetDispatcher(rtcp_dispatcher_.get());
    if (!rtcp_receiver_)
    {
        rtcp_receiver_ = std::make_unique<rtcpx::RtcpReceiverImpl>(rtcp_dispatcher_.get());
    }
    std::weak_ptr<SfuEndpoint> weak_self = weak_from_this();
    const auto pending = std::make_shared<std::atomic<bool>>(false);
    downlinks_.SetSchedule([weak_self, pending] {
        const auto endpoint = weak_self.lock();
        if (!endpoint || !endpoint->IsRunning() || pending->exchange(true)) return;
        if (media_affinity::PostToMediaStream(endpoint->Id(), 0, [weak_self, pending] {
            pending->store(false);
            if (const auto current = weak_self.lock()) current->UpdateDownlinkBandwidth();
        }) != 0) pending->store(false);
    });
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
    return frames_.AddCallback(std::move(cb));
}

void SfuEndpoint::RemoveEncodedFrameCallback(FrameSubscriptionId id)
{
    frames_.RemoveCallback(id);
}

void SfuEndpoint::DispatchEncodedFrame(const media::EncodedFrame::Ptr& frame)
{
    frames_.Dispatch(frame);
}

void SfuEndpoint::OnTrackNack(uint32_t media_ssrc, const std::vector<uint16_t>& lost_seqs)
{
    std::lock_guard<std::recursive_mutex> guard(runtime_mutex_);
    if (!IsRunning()) return;
    if (lost_seqs.empty())
    {
        return;
    }

    auto send = RtcpSenderFor(media_ssrc);
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
    std::lock_guard<std::recursive_mutex> guard(runtime_mutex_);
    if (!IsRunning()) return;
    auto send = RtcpSenderFor(media_ssrc);
    if (!send)
    {
        return;
    }

    const auto binding = ssrc_bindings_.find(media_ssrc);
    auto* limiter = &unbound_keyframe_requests_;
    if (binding != ssrc_bindings_.end())
    {
        auto track = published_tracks_.find(binding->second);
        if (track == published_tracks_.end()) return;
        limiter = &track->second.keyframe_requests;
    }
    if (!limiter->Allow(SteadyNowMs())) return;

    const auto packet = rtcpx::RtcpFeedback::BuildPli(local_rtcp_ssrc_, media_ssrc);
    if (!send(packet.data(), packet.size()))
    {
        LOG_ERROR("[RTCP][PLI] send failed", " media_ssrc=", media_ssrc);
    }
}

bool SfuEndpoint::ScheduleRecoveryTick()
{
    if (!IsRunning()) return false;
    const auto weak = weak_from_this();
    if (weak.expired()) return false;
    return media_affinity::PostToMediaStream(Id(), 0, [weak] {
        if (auto endpoint = weak.lock()) endpoint->TickRecovery();
    }) == 0;
}

void SfuEndpoint::TickRecovery()
{
    std::lock_guard<std::recursive_mutex> guard(runtime_mutex_);
    if (!IsRunning()) return;
    PruneSubscriptions();
    const auto now_ms = SteadyNowMs();
    downlinks_.Tick(now_ms);
    std::vector<rtsp::RtpVideoTracker::Ptr> videos;
    {
        std::lock_guard<std::mutex> lock(track_mtx_);
        for (const auto& entry : ssrc_to_track_)
            if (auto video = std::dynamic_pointer_cast<rtsp::RtpVideoTracker>(entry.second))
                videos.push_back(std::move(video));
    }
    // Callbacks can request a PLI; do not hold track_mtx_ across them.
    for (const auto& video : videos)
    {
        video->UpdateNackRtt(video->GetRttMs());
        video->TickNack(now_ms);
    }
}

void SfuEndpoint::UpdateDownlinkBandwidth()
{
    std::lock_guard<std::recursive_mutex> guard(runtime_mutex_);
    if (IsRunning()) downlinks_.Tick(SteadyNowMs(), true);
}

void SfuEndpoint::Stop()
{
    std::lock_guard<std::recursive_mutex> guard(runtime_mutex_);
    SetState(State::kStopping);
    if (rtcp_dispatcher_)
    {
        rtcp_dispatcher_->SetSenderReportCallback({});
    }

    downlinks_.Clear();
    while (!published_tracks_.empty()) RemovePublishedTrack(published_tracks_.begin()->first);
    receiver_tick_keys_.clear();
    receive_feedback_.Clear();
    SetRtcpSendCallback({});
    SetState(State::kStopped);
}

bool SfuEndpoint::PollMedia(uint64_t stream_key, uint64_t now_ms)
{
    std::vector<media::EncodedFrame::Ptr> frames;
    {
        std::lock_guard<std::recursive_mutex> guard(runtime_mutex_);
        if (!IsRunning()) return false;
        const auto found = receiver_tick_keys_.find(stream_key);
        if (found == receiver_tick_keys_.end()) return false;
        const auto track = found->second.lock();
        if (!track) { receiver_tick_keys_.erase(found); return false; }
        track->TickReceive(now_ms);
        frames.swap(pending_frames_);
    }
    // Match OnRtp: business callbacks run outside the media-state lock.
    for (const auto& frame : frames) DispatchEncodedFrame(frame);
    return true;
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
    std::lock_guard<std::recursive_mutex> guard(runtime_mutex_);
    receiver_tick_keys_.erase(media_affinity::MakeStreamKey(Id(), source_ssrc));
    {
        std::lock_guard<std::mutex> lock(track_mtx_);
        ssrc_to_track_.erase(source_ssrc);
    }
    if (rtcp_dispatcher_)
    {
        rtcp_dispatcher_->RemoveReceiverTrack(source_ssrc);
    }
    receive_feedback_.Remove(source_ssrc);
}

std::optional<NetworkStatsSnapshot> SfuEndpoint::GetReceiveQualitySnapshot(uint32_t source_ssrc) const
{
    std::lock_guard<std::recursive_mutex> guard(runtime_mutex_);
    return receive_feedback_.GetSnapshot(source_ssrc, SteadyNowMs());
}

void SfuEndpoint::EvaluateReceiveQuality(uint32_t source_ssrc)
{
    const auto track = FindReceiverTrackBySsrc(source_ssrc);
    if (receive_feedback_.Evaluate(track, SteadyNowMs(), local_rtcp_ssrc_, RtcpSenderFor(source_ssrc)))
        OnTrackPli(source_ssrc);
}

void SfuEndpoint::ForwardRtpToSubscribers(uint32_t source_ssrc, const uint8_t* data, size_t len)
{
    const auto trace = media_latency::CurrentPacket();
    const auto start = trace ? media_latency::NowNs() : 0;
    const auto binding = ssrc_bindings_.find(source_ssrc);
    if (binding != ssrc_bindings_.end()) 
    {
        published_tracks_.at(binding->second).source_bitrate->Observe(len, SteadyNowMs());
        auto& routes = published_tracks_.at(binding->second).subscribers;
        routes.erase(std::remove_if(routes.begin(), routes.end(), [](const auto& weak) {
            const auto route = weak.lock();
            return !route || route->Closed();
        }), routes.end());
        if (trace) {
            media_latency::Count(routes.empty() ? media_latency::Counter::NoSubscribers : media_latency::Counter::ForwardInputs);
            media_latency::Observe(media_latency::Stage::BeforeForward, trace.worker_ns, start);
        }
        const auto packet = routes.empty() ? common::SharedBuffer{} : common::SharedBuffer::TryCopy(data, len);
        if (!packet.Empty()) for (const auto& weak : routes) 
        {
            const auto route = weak.lock();
            if (!route || !route->CanDeliver()) continue;
            if (!route->source_ssrc) route->source_ssrc = source_ssrc;
            if (*route->source_ssrc != source_ssrc) continue;
            if (auto destination = route->destination.lock()) 
            {
                media_affinity::PostToMediaStream(destination->Id(), route->sender->GetSsrc(),
                    [route, packet, trace] {
                        media_latency::PacketScope scope(trace);
                        if (auto target = route->destination.lock()) target->Deliver(route, packet);
                    });
            }
        }
        if (trace && !routes.empty()) media_latency::Observe(media_latency::Stage::Fanout, start, media_latency::NowNs());
    }
}

}
