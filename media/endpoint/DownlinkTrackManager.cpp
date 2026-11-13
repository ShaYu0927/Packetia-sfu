#include "DownlinkTrackManager.h"
#include "SfuEndpoint.h"
#include "MediaStreamRegistry.h"
#include "MediaStreamAffinity.h"

#include <algorithm>
#include <cassert>
#include <chrono>

namespace media::endpoint
{
namespace
{
uint64_t NowMs()
{
    return std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count();
}

bool IsPadding(const uint8_t* data, size_t size)
{
    if (!data || size < 13 || (data[0] & 0xe0) != 0xa0) return false;
    size_t header = 12 + (data[0] & 15) * 4;
    if (header > size) return false;
    if (data[0] & 0x10)
    {
        if (header + 4 > size) return false;
        header += 4 + ((size_t(data[header + 2]) << 8) | data[header + 3]) * 4;
    }
    return header < size && data[size - 1] && size - header == data[size - 1];
}
// 每个发送轨道的出口闸门，覆盖普通 RTP、缓存重传及发送器产生的 RTCP。
// 不关闭多个轨道共用的底层连接，也不接管底层接收入口。
class ConsumerTransport final : public IMediaTransport
{
public:
    ConsumerTransport(std::shared_ptr<IMediaTransport> transport, const std::shared_ptr<Subscription>& route)
        : transport_(std::move(transport)), route_(route) {}
    uint64_t Id() const noexcept override { return transport_->Id(); }
    MediaTransportProtocol Protocol() const noexcept override { return transport_->Protocol(); }
    MediaTransportState State() const noexcept override
    { const auto route = route_.lock(); return !route || route->Closed() ? MediaTransportState::Closed : transport_->State(); }
    SendResult Send(MediaPacketType type, const uint8_t* data, size_t size, bool retransmit) override
    {
        const auto route = route_.lock();
        if (!route || route->Closed()) return SendResult::Closed;
        if (!route->CanDeliver()) return SendResult::NotWritable;
        if (type == MediaPacketType::Rtp && !route->CanSendMedia() &&
            !(route->probe_allowed.load() && !retransmit && IsPadding(data, size)))
            return SendResult::NotWritable;
        return transport_->SendWithValidity(type, data, size, retransmit,
            Validity(type, !retransmit && IsPadding(data, size)));
    }
    SendResult SendRtpWithCompletion(const uint8_t* data, size_t size, bool retransmit,
                                     std::function<void()> on_sent) override
    {
        const auto route = route_.lock();
        if (!route || route->Closed()) return SendResult::Closed;
        if (!route->CanDeliver()) return SendResult::NotWritable;
        if (!route->CanSendMedia() &&
            !(route->probe_allowed.load() && !retransmit && IsPadding(data, size)))
            return SendResult::NotWritable;
        return transport_->SendWithValidity(MediaPacketType::Rtp, data, size, retransmit,
            Validity(MediaPacketType::Rtp, !retransmit && IsPadding(data, size)), std::move(on_sent));
    }
    void SetPacketSink(std::weak_ptr<IMediaPacketSink>) override {}
    void Close() override { if (const auto route = route_.lock()) route->Close(); }
    std::shared_ptr<media::SendSideController> GetSendSideController() override
    { return transport_->GetSendSideController(); }
private:
    std::function<bool()> Validity(MediaPacketType type, bool padding) const
    {
        return [weak = route_, type, padding] {
            const auto route = weak.lock();
            return route && route->CanDeliver() && (type != MediaPacketType::Rtp ||
                route->CanSendMedia() || (padding && route->probe_allowed.load()));
        };
    }
    const std::shared_ptr<IMediaTransport> transport_;
    const std::weak_ptr<Subscription> route_;
};

}

DownlinkTrackManager::~DownlinkTrackManager() { Clear(); }

void DownlinkTrackManager::SetDispatcher(rtsp::RtcpDispatcher* dispatcher)
{
    assert(subscriptions_.empty() || dispatcher_ == dispatcher);
    dispatcher_ = dispatcher;
}

bool DownlinkTrackManager::Configure(const std::string& id, const rtsp::RtpSenderTrackConfig& config, std::shared_ptr<IMediaTransport> transport, CodecId codec)
{
    Prune();
    if (id.empty() || !transport || codec == CodecId::Unknown || !config.local_ssrc ||
        config.payload_type > 127 || config.sample_rate <= 0 || subscriptions_.count(id)) return false;
    if (config.transport_cc_extension_id > 14 || config.mid_extension_id > 14 ||
        (config.mid_extension_id && (config.mid.empty() || config.mid.size() > 16 ||
                                    config.mid_extension_id == config.transport_cc_extension_id)) ||
        (!config.mid_extension_id && !config.mid.empty())) return false;
    for (const auto& entry : parameters_)
        if (entry.first != id && entry.second.config.local_ssrc == config.local_ssrc) return false;

    if(config.rtx_enabled)
    {
         if (config.rtx_ssrc == 0 || config.rtx_ssrc == config.local_ssrc)
         {
            return false;
         }

         if(config.rtx_payload_type > 127 || config.rtx_payload_type == config.payload_type)
         {
            return false;
         }
    }
    parameters_[id] = {config, std::move(transport), codec};
    parameters_[id].config.rewrite_payload_type = true;
    parameters_[id].config.rewrite_header_extensions = true;
    return true;
}

bool DownlinkTrackManager::Subscribe(const std::string& id, const ::TrackInfo& source_info, MediaTrackRef ref, const std::shared_ptr<Subscription>& route, Callbacks callbacks)
{
    Prune();
    const auto params = parameters_.find(id);
    if (!dispatcher_ || !route || route->Closed() || subscriptions_.count(id) || params == parameters_.end() ||
        params->second.codec != source_info.codec_id ||
        params->second.config.sample_rate != static_cast<int>(source_info.clock_rate)) return false;
    const auto source = route->source.lock();
    const auto destination = route->destination.lock();
    if (!source || !destination) return false;
    const auto transport_id = params->second.transport->Id();
    const auto controller = params->second.transport->GetSendSideController();
    const auto existing = transports_.find(transport_id);
    if (existing != transports_.end() && existing->second.controller != controller) return false;
    if (source_info.ssrc) route->source_ssrc = source_info.ssrc;
    try
    {
        if (ref.Valid())
        {
            auto& registry = MediaStreamRegistry::Instance();
            const auto registered = registry.FindTrack(ref);
            if (!registered || registered->endpoint_id != source->Id() ||
                ref.track_id != static_cast<TrackId>(source_info.track_index)) return false;
            route->consumer = registry.AttachConsumer(ref,
                "rtp:" + std::to_string(destination->Id()) + ":" + id, std::move(callbacks.source_closed));
            if (!route->consumer || route->consumer->Closed()) return false;
        }
        route->transport_id = transport_id;
        route->codec = source_info.codec_id;
        route->audio = source_info.codec_id != CodecId::H264 && source_info.codec_id != CodecId::H265;
        route->send_transport = std::make_shared<ConsumerTransport>(params->second.transport, route);
        route->sender = std::make_shared<rtsp::RtpSenderTrack>(params->second.config, route->send_transport);
        route->sender->SetKeyFrameRequestCallback(std::move(callbacks.request_keyframe));
        subscriptions_.emplace(id, route);
        dispatcher_->AddSenderTrack(route->sender->GetSsrc(), route->sender);
        if (existing == transports_.end())
        {
            auto& group = transports_[transport_id];
            group.controller = controller;
            if (controller && schedule_) group.observer = controller->AddBudgetObserver(schedule_);
        }
    }
    catch (...)
    {
        if (!Unsubscribe(id)) route->Close();
        throw;
    }
    return true;
}

bool DownlinkTrackManager::Unsubscribe(const std::string& id)
{
    const auto it = subscriptions_.find(id);
    if (it == subscriptions_.end()) return false;
    const auto transport_id = it->second->transport_id;
    it->second->Close();
    if (dispatcher_) dispatcher_->RemoveSenderTrack(it->second->sender->GetSsrc());
    subscriptions_.erase(it);
    const auto group = transports_.find(transport_id);
    if (group != transports_.end())
    {
        if (group->second.probe_track == id)
            FinishProbe(group->second, NowMs(), ProbeEventType::Aborted);
        group->second.allocator.Remove(id);
        const bool retained = std::any_of(subscriptions_.begin(), subscriptions_.end(), [&](const auto& entry) {
            return entry.second->transport_id == transport_id;
        });
        if (!retained)
        {
            if (group->second.controller) group->second.controller->RemoveBudgetObserver(group->second.observer);
            transports_.erase(group);
        }
    }
    if (schedule_) schedule_();
    return true;
}

void DownlinkTrackManager::Prune()
{
    for (auto it = subscriptions_.begin(); it != subscriptions_.end();)
    {
        if (it->second->Closed() || it->second->source.expired())
        {
            const auto id = it++->first;
            Unsubscribe(id);
        }
        else ++it;
    }
}

void DownlinkTrackManager::Clear()
{
    while (!subscriptions_.empty()) Unsubscribe(subscriptions_.begin()->first);
    parameters_.clear();
    next_tick_ms_ = 0;
}

size_t DownlinkTrackManager::Count() const
{
    return std::count_if(subscriptions_.begin(), subscriptions_.end(),
        [](const auto& entry) { return !entry.second->Closed(); });
}

void DownlinkTrackManager::Deliver(const std::shared_ptr<Subscription>& route, common::BufferView packet)
{
    const auto now_ms = NowMs();
    Tick(now_ms);
    if (route && route->CanDeliver() && !packet.Empty())
    {
        const auto state = route->bandwidth_state.load();
        if (state != DownlinkBandwidthState::Forwarding)
        {
            auto recovery = route->recovery.Push(route->codec, packet.Data(), packet.Size(), now_ms);
            if (state == DownlinkBandwidthState::Paused || recovery.empty()) return;
            route->sender->BeginDiscontinuity();
            route->bandwidth_state.store(DownlinkBandwidthState::Forwarding);
            for (const auto& part : recovery)
            {
                if (!route->sender->InputRtpPacket(part.data(), part.size()))
                {
                    route->bandwidth_state.store(DownlinkBandwidthState::WaitingKeyFrame);
                    route->sender->BeginDiscontinuity();
                    route->next_keyframe_request_ms = 0;
                    break;
                }
            }
            return;
        }
        media_latency::Count(media_latency::Counter::SenderAttempts);
        if (!route->sender->InputRtpPacket(packet.Data(), packet.Size()))
            media_latency::Count(media_latency::Counter::SenderRejected);
    }
}

uint32_t DownlinkTrackManager::RequiredBitrate(const Subscription& route) const
{
    const auto measured = route.source_bitrate ? route.source_bitrate->Bitrate() : 0;
    return measured ? measured : route.bandwidth_policy.bitrate_hint_bps ?
        route.bandwidth_policy.bitrate_hint_bps : route.audio ? 32000 : 500000;
}

bool DownlinkTrackManager::SetAudioMuted(const std::string& id, bool muted, bool& changed)
{
    changed = false;
    const auto it = subscriptions_.find(id);
    if (it == subscriptions_.end() || it->second->Closed() || !it->second->audio || !it->second->consumer)
        return false;
    const auto& consumer = it->second->consumer;
    const bool previous = consumer->Paused();
    if (!MediaStreamRegistry::Instance().SetConsumerPaused(consumer->Id(), muted)) return false;
    changed = previous != muted;
    return true;
}

bool DownlinkTrackManager::SetPolicy(const std::string& id, const DownlinkSubscriptionPolicy& policy)
{
    const auto it = subscriptions_.find(id);
    if (it == subscriptions_.end() || it->second->Closed()) return false;
    it->second->bandwidth_policy = policy;
    Tick(NowMs(), true);
    return true;
}

std::optional<DownlinkBandwidthStatus> DownlinkTrackManager::GetStatus(const std::string& id) const
{
    const auto it = subscriptions_.find(id);
    if (it == subscriptions_.end() || it->second->Closed()) return std::nullopt;
    return DownlinkBandwidthStatus{it->second->bandwidth_state.load(),
        it->second->bandwidth_policy, RequiredBitrate(*it->second)};
}

void DownlinkTrackManager::Tick(uint64_t now_ms, bool force)
{
    if (!force && now_ms < next_tick_ms_) return;
    next_tick_ms_ = now_ms + 20;
    // Controller callbacks can re-enter subscription management. Run them
    // before iterating transport groups, retaining only controller ownership.
    std::vector<std::shared_ptr<SendSideController>> controllers;
    controllers.reserve(transports_.size());
    for (const auto& entry : transports_)
        if (entry.second.controller) controllers.push_back(entry.second.controller);
    for (const auto& controller : controllers) controller->OnProcessInterval(now_ms);
    for (auto& entry : transports_)
    {
        auto& group = entry.second;
        if (!group.controller) continue;
        const auto budget = group.controller->GetBandwidthBudget();
        std::vector<DownlinkDemand> demands;
        for (const auto& subscription : subscriptions_)
        {
            const auto& route = subscription.second;
            if (route->transport_id != entry.first) continue;
            if (group.path_epoch && group.path_epoch != budget.path_epoch && !route->audio)
            {
                route->sender->BeginDiscontinuity();
                route->recovery.Reset();
                route->next_keyframe_request_ms = 0;
                if (route->bandwidth_state.load() == DownlinkBandwidthState::Forwarding)
                    route->bandwidth_state.store(DownlinkBandwidthState::WaitingKeyFrame);
            }
            // Policy-exempt tracks keep flowing, but still reserve their cost.
            demands.push_back({subscription.first, route->audio || !route->bandwidth_policy.enabled,
                route->CanDeliver(), route->bandwidth_policy.priority, RequiredBitrate(*route)});
        }
        if (group.path_epoch != budget.path_epoch || group.probe_generation != budget.probe_generation)
        {
            FinishProbe(group, now_ms, ProbeEventType::Aborted);
            group.last_probe_ms.reset();
        }
        group.path_epoch = budget.path_epoch;
        group.probe_generation = budget.probe_generation;
        const auto allocations = group.allocator.Allocate(budget, demands, now_ms);
        for (const auto& allocation : allocations)
        {
            const auto& route = subscriptions_.at(allocation.id);
            if (route->audio) continue;
            const auto state = route->bandwidth_state.load();
            const bool allowed = !route->bandwidth_policy.enabled || allocation.allowed;
            if (!allowed && state != DownlinkBandwidthState::Paused)
            {
                route->bandwidth_state.store(DownlinkBandwidthState::Paused);
                route->sender->BeginDiscontinuity();
                route->recovery.Reset();
            }
            else if (allowed && state == DownlinkBandwidthState::Paused)
            {
                route->bandwidth_state.store(DownlinkBandwidthState::WaitingKeyFrame);
                route->recovery.Restart();
                route->next_keyframe_request_ms = 0;
            }
            if (route->bandwidth_state.load() == DownlinkBandwidthState::WaitingKeyFrame &&
                route->CanDeliver() && budget.network_available && now_ms >= route->next_keyframe_request_ms)
            {
                route->next_keyframe_request_ms = now_ms + 1000;
                route->sender->OnRtcpPli();
            }
        }
        Probe(group, budget, demands, now_ms);
    }
}

void DownlinkTrackManager::Probe(TransportGroup& group, const DownlinkBandwidthBudget& budget,
    const std::vector<DownlinkDemand>& demands, uint64_t now_ms)
{
    if (!budget.network_available)
    {
        FinishProbe(group, now_ms, ProbeEventType::Aborted);
        return;
    }
    if (group.probe_started_ms && (now_ms < *group.probe_started_ms ||
        now_ms - *group.probe_started_ms >= group.probe_plan->duration_ms))
    {
        const auto type = now_ms < *group.probe_started_ms ? ProbeEventType::Aborted : ProbeEventType::Finished;
        FinishProbe(group, now_ms, type);
    }
    if (!group.probe_started_ms)
    {
        const DownlinkDemand* candidate = nullptr;
        uint64_t active_bps = 0;
        for (const auto& demand : demands)
        {
            const auto& route = subscriptions_.at(demand.id);
            if (!demand.eligible) continue;
            if (route->bandwidth_state.load() != DownlinkBandwidthState::Paused)
                active_bps += demand.required_bps;
            else if (route->bandwidth_policy.enabled && parameters_.at(demand.id).config.transport_cc_extension_id &&
                (!candidate || demand.priority > candidate->priority ||
                 (demand.priority == candidate->priority && demand.id < candidate->id))) candidate = &demand;
        }
        if (!candidate) return;
        const auto decision = group.controller->GetProbeDecision(
            {now_ms, budget.path_epoch, active_bps, candidate->required_bps});
        ProbePlan plan;
        if (decision.handled)
        {
            if (!decision.plan) return;
            plan = *decision.plan;
        }
        else
        {
            // 保留原有策略；新 ProbeController 的核心算法由调用方实现。
            if (group.last_probe_ms && (now_ms < *group.last_probe_ms || now_ms - *group.last_probe_ms < 3000)) return;
            const uint64_t ceiling = std::min<uint64_t>(budget.max_bps, budget.target_bps ?
                std::max<uint64_t>(uint64_t(budget.target_bps) * 3 / 2, uint64_t(budget.target_bps) + 64000) : budget.start_bps);
            const auto total = std::min<uint64_t>(ceiling, (active_bps + candidate->required_bps) * 150 / 100);
            if (total <= active_bps) return;
            plan = {static_cast<uint32_t>(total - active_bps), 500, 128 * 1024};
        }
        // 执行边界独立于算法：限制额外速率、单簇时长和总字节数。
        if (!plan.bitrate_bps || !plan.duration_ms || plan.max_bytes < 300 || active_bps >= budget.max_bps) return;
        plan.bitrate_bps = static_cast<uint32_t>(std::min<uint64_t>(plan.bitrate_bps, budget.max_bps - active_bps));
        plan.duration_ms = std::min<uint32_t>(plan.duration_ms, 500);
        plan.max_bytes = std::min<uint64_t>(plan.max_bytes, 128 * 1024);
        plan.generation = budget.probe_generation;
        group.probe_plan = group.controller->StartProbe(plan, now_ms, budget.path_epoch);
        if (!group.probe_plan) return;
        group.probe_started_ms = group.last_probe_ms = now_ms;
        group.probe_track = candidate->id;
        group.probe_bytes = 0;
    }
    const auto it = subscriptions_.find(group.probe_track);
    if (it == subscriptions_.end() || !it->second->CanDeliver() ||
        it->second->bandwidth_state.load() != DownlinkBandwidthState::Paused)
    { FinishProbe(group, now_ms, ProbeEventType::Aborted); return; }
    const uint64_t elapsed = now_ms - *group.probe_started_ms + 20;
    const auto allowance = std::min<uint64_t>(group.probe_plan->max_bytes,
        uint64_t(group.probe_plan->bitrate_bps) * elapsed / 8000);
    const auto& route = it->second;
    struct ProbePermit
    {
        std::atomic<bool>& allowed;
        explicit ProbePermit(std::atomic<bool>& flag) : allowed(flag) { allowed.store(true); }
        ~ProbePermit() { allowed.store(false); }
    } permit(route->probe_allowed);
    // Bound bursts when the worker tick is delayed. Failed sends consume no credit.
    for (size_t sent = 0; sent < 16384 && group.probe_bytes + 300 <= allowance; sent += 300)
    {
        if (!route->sender->SendPadding(240, group.probe_plan->cluster_id)) break;
        group.probe_bytes += 300; // Includes a conservative header/crypto allowance.
    }
    if (group.probe_bytes + 300 > group.probe_plan->max_bytes)
        FinishProbe(group, now_ms, ProbeEventType::Finished);
}

void DownlinkTrackManager::FinishProbe(TransportGroup& group, uint64_t now_ms, ProbeEventType type)
{
    if (group.probe_plan)
        group.controller->OnProbeEvent({type, *group.probe_plan, group.path_epoch, now_ms, group.probe_bytes});
    group.probe_started_ms.reset();
    group.probe_plan.reset();
    group.probe_track.clear();
    group.probe_bytes = 0;
}
}
