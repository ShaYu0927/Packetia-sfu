#include "MediaEndpoint.h"
#include "MediaStreamAffinity.h"
#include "SdpTrackBinding.h"

#include <algorithm>

namespace media
{
namespace
{
// Parse both RFC 8285 formats. Unknown extension profiles cannot establish a
// MID binding; a previously negotiated SSRC can still identify the track.
bool ReadExtensions(common::BufferView packet, std::unordered_map<uint8_t, std::string>& values)
{
    const auto* p = packet.Data();
    if (!p || packet.Size() < 12 || (p[0] >> 6) != 2) return false;
    size_t offset = 12 + 4 * (p[0] & 15);
    if (offset > packet.Size()) return false;
    if (p[0] & 0x10) {
        if (offset + 4 > packet.Size()) return false;
        const uint16_t profile = media_affinity::ReadUint16BE(p + offset);
        const size_t end = offset + 4 + 4 * media_affinity::ReadUint16BE(p + offset + 2);
        offset += 4;
        if (end > packet.Size()) return false;
        if (profile == 0xBEDE || (profile & 0xFFF0) == 0x1000) {
            while (offset < end) {
                uint8_t id = p[offset++];
                if (id == 0) continue;
                size_t size;
                if (profile == 0xBEDE) {
                    size = (id & 15) + 1;
                    id >>= 4;
                    if (id == 15) break;
                } else {
                    if (offset == end) return false;
                    size = p[offset++];
                }
                if (offset + size > end || values.count(id)) return false;
                values.emplace(id, std::string(reinterpret_cast<const char*>(p + offset), size));
                offset += size;
            }
        }
        offset = end;
    }
    if (p[0] & 0x20) {
        const size_t padding = p[packet.Size() - 1];
        if (!padding || padding > packet.Size() - offset) return false;
    }
    return true;
}
}

bool SfuEndpoint::AddPublishedTrack(const std::string& id,
                                    std::shared_ptr<RtpTrackDescription> description,
                                    std::string mid, uint8_t mid_extension_id)
{
    std::lock_guard<std::recursive_mutex> guard(runtime_mutex_);
    if (id.empty() || !description || GetState() == State::kStopped ||
        GetState() == State::kStopping || published_tracks_.count(id) || retired_track_ids_.count(id)) return false;
    const auto& info = description->getTrackInfo();
    if (info.track_index < 0 || info.clock_rate == 0 || info.payload_type > 127 ||
        (info.ssrc && ssrc_bindings_.count(info.ssrc))) return false;
    for (const auto& entry : published_tracks_) {
        if (entry.second.description->getTrackIndex() == info.track_index ||
            (!mid.empty() && entry.second.mid == mid)) return false;
    }
    PublishedTrack track;
    track.description = std::make_shared<RtpTrackDescription>(info);
    track.mid = std::move(mid);
    track.mid_extension_id = mid_extension_id;
    published_tracks_.emplace(id, std::move(track));
    if (info.ssrc) { ssrc_bindings_.emplace(info.ssrc, id); retired_ssrcs_.erase(info.ssrc); }
    return true;
}

bool SfuEndpoint::BindSsrc(const std::string& id, uint32_t ssrc)
{
    std::lock_guard<std::recursive_mutex> guard(runtime_mutex_);
    if (!published_tracks_.count(id) || GetState() == State::kStopped) return false;
    const auto result = ssrc_bindings_.emplace(ssrc, id);
    if (result.second || result.first->second == id) retired_ssrcs_.erase(ssrc);
    return result.second || result.first->second == id;
}

bool SfuEndpoint::AddPublishedTrack(const std::string& id, const sdp::SdpMedia& media, int track_index)
{
    std::lock_guard<std::recursive_mutex> guard(runtime_mutex_);
    ::TrackInfo info;
    if (!BuildRtpTrackInfo(media, track_index, info)) return false;
    // Repair streams need decapsulation before entering a primary receiver.
    // The current WebRTC answerer also excludes RTX/RED/FEC.
    for (const auto& group : media.ssrcGroups)
        if (group.semantics == "FID" || group.semantics == "FEC" || group.semantics == "FEC-FR") return false;
    for (const auto& ssrc : media.ssrcs)
        if (ssrc_bindings_.count(ssrc.ssrc)) return false;
    if (!AddPublishedTrack(id, std::make_shared<RtpTrackDescription>(info), media.mid,
        FindRtpExtensionId(media, "urn:ietf:params:rtp-hdrext:sdes:mid"))) return false;
    for (const auto& ssrc : media.ssrcs) BindSsrc(id, ssrc.ssrc);
    return true;
}

bool SfuEndpoint::GetPublishedTrack(const std::string& id, ::TrackInfo& info) const
{
    std::lock_guard<std::recursive_mutex> guard(runtime_mutex_);
    const auto it = published_tracks_.find(id);
    if (it == published_tracks_.end()) return false;
    info = it->second.description->getTrackInfo();
    return true;
}

size_t SfuEndpoint::PublishedTrackCount() const
{
    std::lock_guard<std::recursive_mutex> guard(runtime_mutex_);
    return published_tracks_.size();
}

size_t SfuEndpoint::SubscriptionCount() const
{
    std::lock_guard<std::recursive_mutex> guard(runtime_mutex_);
    return std::count_if(subscriptions_.begin(), subscriptions_.end(),
        [](const auto& entry) { return entry.second->active.load(); });
}

bool SfuEndpoint::RemovePublishedTrack(const std::string& id, bool retire_identity)
{
    std::lock_guard<std::recursive_mutex> guard(runtime_mutex_);
    const auto it = published_tracks_.find(id);
    if (it == published_tracks_.end()) return false;
    for (const auto& weak : it->second.subscribers) {
        if (auto route = weak.lock()) {
            route->active.store(false);
            if (auto destination = route->destination.lock()) {
                const std::weak_ptr<SfuEndpoint> target = destination;
                WorkerService::post_fn(POOL_MEDIA, destination->Id(), [target] {
                    if (auto endpoint = target.lock()) {
                        std::lock_guard<std::recursive_mutex> lock(endpoint->runtime_mutex_);
                        endpoint->PruneSubscriptions();
                    }
                });
            }
        }
    }
    for (auto binding = ssrc_bindings_.begin(); binding != ssrc_bindings_.end();) {
        if (binding->second == id) {
            RemoveMediaStreamOnOwner(binding->first);
            if (retire_identity) retired_ssrcs_.insert(binding->first);
            binding = ssrc_bindings_.erase(binding);
        } else ++binding;
    }
    if (retire_identity) retired_track_ids_.insert(id);
    published_tracks_.erase(it);
    return true;
}

bool SfuEndpoint::ResolveRtpTrack(common::BufferView packet, const std::string& hint)
{
    std::unordered_map<uint8_t, std::string> extensions;
    if (!ReadExtensions(packet, extensions)) return false;
    const uint32_t ssrc = media_affinity::ReadUint32BE(packet.Data() + 8);
    if (retired_ssrcs_.count(ssrc)) return false;
    const uint8_t pt = packet.Data()[1] & 127;
    std::string selected = hint;
    const auto bound = ssrc_bindings_.find(ssrc);
    if (bound != ssrc_bindings_.end()) {
        if (!selected.empty() && selected != bound->second) return false;
        selected = bound->second;
    }
    bool has_mid = false;
    std::string mid_track;
    for (const auto& entry : published_tracks_) {
        const auto ext = extensions.find(entry.second.mid_extension_id);
        if (!entry.second.mid_extension_id || ext == extensions.end()) continue;
        has_mid = true;
        if (ext->second == entry.second.mid) {
            if (!mid_track.empty() && mid_track != entry.first) return false;
            mid_track = entry.first;
        }
    }
    if (has_mid) {
        if (mid_track.empty() || (!selected.empty() && selected != mid_track)) return false;
        selected = mid_track;
    }
    if (selected.empty()) {
        for (const auto& entry : published_tracks_) {
            if (entry.second.description->getPayloadType() != pt) continue;
            if (!selected.empty()) return false; // Ambiguous PT cannot bind a new SSRC.
            selected = entry.first;
        }
    }
    const auto track = published_tracks_.find(selected);
    if (track == published_tracks_.end() || track->second.description->getPayloadType() != pt) return false;
    return BindSsrc(selected, ssrc);
}

void SfuEndpoint::OnRtp(WorkJob& job)
{
    std::vector<media::EncodedFrame::Ptr> frames;
    {
        std::lock_guard<std::recursive_mutex> guard(runtime_mutex_);
        if (!IsRunning() || job.payload.Empty() || !ResolveRtpTrack(job.payload.View(), job.media_track_id)) return;
        MediaEndpoint::OnRtp(job);
        frames.swap(pending_frames_);
    }
    // Business/recording callbacks can enter Room or change subscriptions.
    // Never invoke them while holding the endpoint's media-state lock.
    for (const auto& frame : frames) DispatchEncodedFrame(frame);
}

void SfuEndpoint::OnRtcp(WorkJob& job)
{
    std::lock_guard<std::recursive_mutex> guard(runtime_mutex_);
    if (!IsRunning()) return;
    if (!job.media_track_id.empty() && !published_tracks_.count(job.media_track_id)) return;
    // An explicit transport binding also lets an SR preceding the first RTP
    // establish the source. Never infer a source from a feedback media SSRC.
    if (!job.media_track_id.empty() && job.payload.Size() >= 8 && job.payload.Data()[1] == 200) {
        rtcpx::RtcpPacketInfo info;
        if (!rtcpx::InspectRtcpPacket(job.payload.Data(), job.payload.Size(), &info)) return;
        const auto ssrc = media_affinity::ReadUint32BE(job.payload.Data() + 4);
        if (retired_ssrcs_.count(ssrc) || !BindSsrc(job.media_track_id, ssrc)) return;
    }
    MediaEndpoint::OnRtcp(job);
}

void SfuEndpoint::SetTrackRtcpSendCallback(const std::string& id, SendRtcpCallback cb)
{
    std::lock_guard<std::recursive_mutex> guard(runtime_mutex_);
    const auto it = published_tracks_.find(id);
    if (it != published_tracks_.end()) it->second.send_rtcp = std::move(cb);
}

SfuEndpoint::SendRtcpCallback SfuEndpoint::RtcpSenderFor(uint32_t ssrc)
{
    const auto binding = ssrc_bindings_.find(ssrc);
    if (binding != ssrc_bindings_.end()) {
        const auto track = published_tracks_.find(binding->second);
        if (track != published_tracks_.end() && track->second.send_rtcp) return track->second.send_rtcp;
    }
    std::lock_guard<std::mutex> lock(rtcp_send_mutex_);
    return send_rtcp_cb_;
}

bool SfuEndpoint::ConfigureSubscription(const std::string& id,
                                       const rtsp::RtpSenderTrackConfig& config,
                                       std::shared_ptr<IMediaTransport> transport, CodecId codec)
{
    std::lock_guard<std::recursive_mutex> guard(runtime_mutex_);
    PruneSubscriptions();
    if (id.empty() || !transport || codec == CodecId::Unknown || !config.local_ssrc || config.payload_type > 127 ||
        config.sample_rate <= 0 || subscriptions_.count(id) || GetState() == State::kStopped) return false;
    if (config.transport_cc_extension_id > 14 || config.mid_extension_id > 14 ||
        (config.mid_extension_id && (config.mid.empty() || config.mid.size() > 16 ||
                                    config.mid_extension_id == config.transport_cc_extension_id)) ||
        (!config.mid_extension_id && !config.mid.empty())) return false;
    for (const auto& entry : subscription_parameters_)
        if (entry.first != id && entry.second.config.local_ssrc == config.local_ssrc) return false;
    subscription_parameters_[id] = {config, std::move(transport), codec};
    subscription_parameters_[id].config.rewrite_payload_type = true;
    subscription_parameters_[id].config.rewrite_header_extensions = true;
    return true;
}

bool SfuEndpoint::Subscribe(const std::string& id, const std::shared_ptr<SfuEndpoint>& source,
                            const std::string& source_track_id)
{
    if (!source || source.get() == this) return false;
    std::scoped_lock lock(runtime_mutex_, source->runtime_mutex_);
    PruneSubscriptions();
    const auto params = subscription_parameters_.find(id);
    const auto track = source->published_tracks_.find(source_track_id);
    if (!IsRunning() || !source->IsRunning() || subscriptions_.count(id) ||
        params == subscription_parameters_.end() || track == source->published_tracks_.end()) return false;
    // This path forwards a single encoding. Selecting among simulcast/RTX
    // streams requires a separate layer policy and is deliberately explicit.
    const auto& source_info = track->second.description->getTrackInfo();
    if (params->second.codec != source_info.codec_id ||
        params->second.config.sample_rate != static_cast<int>(source_info.clock_rate)) return false;
    auto route = std::make_shared<Subscription>();
    route->source = source;
    route->destination = shared_from_this();
    route->source_track_id = source_track_id;
    if (source_info.ssrc) route->source_ssrc = source_info.ssrc;
    route->sender = std::make_shared<rtsp::RtpSenderTrack>(params->second.config, params->second.transport);
    const std::weak_ptr<Subscription> weak = route;
    route->sender->SetKeyFrameRequestCallback([weak] {
        if (const auto route = weak.lock(); route && route->active.load()) {
            if (const auto upstream = route->source.lock()) {
                WorkerService::post_fn(POOL_MEDIA, upstream->Id(), [weak] {
                    if (const auto current = weak.lock(); current && current->active.load())
                        if (auto publisher = current->source.lock()) publisher->RequestKeyFrame(current->source_track_id);
                });
            }
        }
    });
    rtcp_dispatcher_->AddSenderTrack(route->sender->GetSsrc(), route->sender);
    subscriptions_.emplace(id, route);
    track->second.subscribers.push_back(route);
    return true;
}

bool SfuEndpoint::Unsubscribe(const std::string& id)
{
    std::lock_guard<std::recursive_mutex> guard(runtime_mutex_);
    const auto it = subscriptions_.find(id);
    if (it == subscriptions_.end()) return false;
    it->second->active.store(false);
    if (rtcp_dispatcher_) rtcp_dispatcher_->RemoveSenderTrack(it->second->sender->GetSsrc());
    subscriptions_.erase(it);
    return true;
}

void SfuEndpoint::PruneSubscriptions()
{
    for (auto it = subscriptions_.begin(); it != subscriptions_.end();) {
        if (!it->second->active.load() || it->second->source.expired()) {
            const auto id = it++->first;
            Unsubscribe(id);
        } else ++it;
    }
}

void SfuEndpoint::Deliver(const std::shared_ptr<Subscription>& route, common::SharedBuffer packet)
{
    std::lock_guard<std::recursive_mutex> guard(runtime_mutex_);
    if (IsRunning() && route->active.load() && !packet.Empty()) {
        media_latency::Count(media_latency::Counter::SenderAttempts);
        if (!route->sender->InputRtpPacket(packet.Data(), packet.Size()))
            media_latency::Count(media_latency::Counter::SenderRejected);
    }
}

void SfuEndpoint::RequestKeyFrame(const std::string& track_id)
{
    std::lock_guard<std::recursive_mutex> guard(runtime_mutex_);
    if (!IsRunning()) return;
    for (const auto& binding : ssrc_bindings_)
        if (binding.second == track_id) OnTrackPli(binding.first);
}
} // namespace media
