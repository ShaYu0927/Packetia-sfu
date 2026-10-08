#include "MediaEndpoint.h"
#include "MediaStreamAffinity.h"
#include "SdpTrackBinding.h"
#include "../../Rtsp/Rtp/RtpHeaderExtensions.h"

#include <algorithm>
#include <limits>

namespace media
{
namespace
{
bool SameEncoding(const ::TrackInfo& a, const ::TrackInfo& b)
{
    return a.type == b.type && a.codec_id == b.codec_id && a.clock_rate == b.clock_rate &&
        std::max(1, a.channels) == std::max(1, b.channels) && a.fmtp == b.fmtp;
}

bool SameReceiver(const ::TrackInfo& a, const ::TrackInfo& b)
{
    return SameEncoding(a, b) && a.payload_type == b.payload_type &&
        a.transport_cc_extension_id == b.transport_cc_extension_id;
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

bool SfuEndpoint::ReplacePublishedTracks(const std::vector<sdp::SdpMedia>& medias)
try
{
    struct Candidate
    {
        ::TrackInfo info;
        std::string mid;
        uint8_t mid_extension = 0;
        std::vector<uint32_t> ssrcs;
    };
    std::vector<Candidate> candidates;
    std::unordered_set<std::string> mids;
    std::unordered_set<uint32_t> offeredSsrcs;
    for (const auto& media : medias)
    {
        if (media.mid.empty() || !mids.insert(media.mid).second || media.codecs.size() != 1) return false;
        const auto& codec = media.codecs.front();
        if (codec.payloadType < 0 || codec.payloadType > 127 || codec.clockRate <= 0 || codec.channels < 0)
            return false;
        for (const auto& group : media.ssrcGroups)
            if (group.semantics == "FID" || group.semantics == "FEC" || group.semantics == "FEC-FR") return false;
        Candidate candidate;
        if (!BuildRtpTrackInfo(media, 0, candidate.info)) return false;
        candidate.mid = media.mid;
        candidate.mid_extension = FindRtpExtensionId(media, "urn:ietf:params:rtp-hdrext:sdes:mid");
        for (const auto& source : media.ssrcs)
        {
            if (!source.ssrc || !offeredSsrcs.insert(source.ssrc).second) return false;
            candidate.ssrcs.push_back(source.ssrc);
        }
        candidates.push_back(std::move(candidate));
    }

    std::vector<std::weak_ptr<SfuEndpoint>> prune;
    {
        std::lock_guard<std::recursive_mutex> guard(runtime_mutex_);
        if (GetState() == State::kStopped || GetState() == State::kStopping) return false;
        std::unordered_map<std::string, PublishedTrack> tracks;
        std::unordered_map<uint32_t, std::string> bindings;
        auto paused = paused_published_tracks_;
        auto retired = retired_ssrcs_;
        std::unordered_set<std::string> reset;
        std::vector<std::shared_ptr<Subscription>> deactivate;
        int nextIndex = 0;
        const auto reserveIndex = [&](const PublishedTrack& track)
        {
            const int index = track.description->getTrackIndex();
            if (index == std::numeric_limits<int>::max()) return false;
            nextIndex = std::max(nextIndex, index + 1);
            return true;
        };
        for (const auto& entry : published_tracks_) if (!reserveIndex(entry.second)) return false;
        for (const auto& entry : paused) if (!reserveIndex(entry.second.track)) return false;

        for (auto& candidate : candidates)
        {
            std::string id = candidate.mid;
            const PublishedTrack* previous = nullptr;
            const PausedPublishedTrack* suspended = nullptr;
            for (const auto& entry : published_tracks_)
                if (entry.second.mid == candidate.mid || (entry.second.mid.empty() && entry.first == candidate.mid))
                {
                    id = entry.first;
                    previous = &entry.second;
                    break;
                }
            if (!previous)
                for (const auto& entry : paused)
                    if (entry.second.track.mid == candidate.mid)
                    {
                        id = entry.first;
                        previous = &entry.second.track;
                        suspended = &entry.second;
                        break;
                    }
            if (tracks.count(id) || (!previous && (published_tracks_.count(id) || paused.count(id) ||
                retired_track_ids_.count(id)))) return false;
            if (!previous && nextIndex == std::numeric_limits<int>::max()) return false;
            candidate.info.track_index = previous ? previous->description->getTrackIndex() : nextIndex++;
            PublishedTrack track;
            if (previous)
            {
                track = *previous;
                const auto& oldInfo = previous->description->getTrackInfo();
                if (!SameEncoding(oldInfo, candidate.info))
                    for (const auto& weak : previous->subscribers)
                        if (const auto route = weak.lock(); route && route->active.load()) return false;
                if (!SameReceiver(oldInfo, candidate.info)) reset.insert(id);
            }
            track.description = std::make_shared<RtpTrackDescription>(candidate.info);
            track.mid = candidate.mid;
            track.mid_extension_id = candidate.mid_extension;
            if (candidate.ssrcs.empty())
            {
                if (suspended) candidate.ssrcs = suspended->ssrcs;
                else
                    for (const auto& binding : ssrc_bindings_)
                        if (binding.second == id) candidate.ssrcs.push_back(binding.first);
            }
            for (const auto ssrc : candidate.ssrcs)
            {
                const auto old = ssrc_bindings_.find(ssrc);
                if (old != ssrc_bindings_.end() && old->second != id) return false;
                if (retired_ssrcs_.count(ssrc) && (!suspended ||
                    std::find(suspended->ssrcs.begin(), suspended->ssrcs.end(), ssrc) == suspended->ssrcs.end()))
                    return false;
                for (const auto& entry : paused)
                    if (entry.first != id && std::find(entry.second.ssrcs.begin(), entry.second.ssrcs.end(), ssrc)
                        != entry.second.ssrcs.end()) return false;
                if (!bindings.emplace(ssrc, id).second) return false;
                retired.erase(ssrc);
            }
            for (const auto& weak : track.subscribers)
                if (const auto route = weak.lock(); route && route->active.load() && route->source_ssrc &&
                    std::find(candidate.ssrcs.begin(), candidate.ssrcs.end(), *route->source_ssrc) == candidate.ssrcs.end())
                    // The current sender cannot translate a new source's
                    // sequence/timestamp base into the existing output SSRC.
                    return false;
            tracks.emplace(id, std::move(track));
            paused.erase(id);
        }

        for (const auto& entry : published_tracks_)
        {
            if (tracks.count(entry.first)) continue;
            PausedPublishedTrack saved;
            saved.track = entry.second;
            for (const auto& binding : ssrc_bindings_)
                if (binding.second == entry.first) saved.ssrcs.push_back(binding.first);
            for (const auto& weak : entry.second.subscribers)
                if (const auto route = weak.lock(); route && route->active.load())
                {
                    deactivate.push_back(route);
                    prune.push_back(route->destination);
                }
            saved.track.subscribers.clear();
            paused.insert_or_assign(entry.first, std::move(saved));
        }
        std::vector<uint32_t> removeReceivers;
        for (const auto& binding : ssrc_bindings_)
        {
            if (!bindings.count(binding.first)) retired.insert(binding.first);
            if (!bindings.count(binding.first) || reset.count(binding.second)) removeReceivers.push_back(binding.first);
        }

        // All validation and allocations precede this commit. Receiver state
        // and route flags change while packets remain excluded by this lock.
        for (const auto ssrc : removeReceivers) RemoveMediaStreamOnOwner(ssrc);
        for (const auto& route : deactivate) route->active.store(false);
        published_tracks_.swap(tracks);
        paused_published_tracks_.swap(paused);
        ssrc_bindings_.swap(bindings);
        retired_ssrcs_.swap(retired);
    }
    // Route flags already prevent delivery. Reclaim destination senders on
    // their owners; their next RTCP/control operation also prunes them.
    for (const auto& target : prune)
        try
        {
            if (const auto endpoint = target.lock())
                WorkerService::post_fn(POOL_MEDIA, endpoint->Id(), [target]
                {
                    if (const auto endpoint = target.lock())
                    {
                        std::lock_guard<std::recursive_mutex> guard(endpoint->runtime_mutex_);
                        endpoint->PruneSubscriptions();
                    }
                });
        }
        catch (const std::bad_alloc&) {}
    return true;
}
catch (const std::bad_alloc&) { return false; }

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
    if (!rtsp::ReadRtpHeaderExtensions(packet.Data(), packet.Size(), extensions)) return false;
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
