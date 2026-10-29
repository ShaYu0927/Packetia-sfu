#include "EncodedFrameRouter.h"

#include <new>
#include <stdexcept>
#include <utility>

namespace media
{
EncodedFrameFilter EncodedFrameFilter::ForStream(uint32_t stream)
{
    if (!stream) throw std::invalid_argument("media stream ID must be nonzero");
    EncodedFrameFilter filter;
    filter.media_stream_id = stream;
    return filter;
}

EncodedFrameFilter EncodedFrameFilter::ForTrack(MediaTrackRef track)
{
    auto filter = ForStream(track.stream_id);
    filter.track_id = track.track_id;
    return filter;
}

bool EncodedFrameFilter::Valid() const noexcept
{
    // 单独的流内轨道编号不足以定位来源，必须同时指定发布实例或端点。
    return !track_id || media_stream_id || endpoint_id;
}

bool EncodedFrameFilter::Matches(const EncodedFrameSource& source) const noexcept
{
    return (!media_stream_id || media_stream_id == source.media_stream_id) &&
        (!endpoint_id || endpoint_id == source.endpoint_id) &&
        (!track_id || *track_id == source.track_id) &&
        (session_id.empty() || session_id == source.session_id) &&
        (stream_id.empty() || stream_id == source.stream_id) &&
        (!ssrc || ssrc == source.ssrc);
}

std::shared_ptr<const EncodedFrameRouter::Routes> EncodedFrameRouter::BuildRoutes(const Slots& slots)
{
    auto routes = std::make_shared<Routes>();
    for (const auto& entry : slots)
    {
        const auto& slot = entry.second;
        if (!slot->active.load() || slot->sink.expired() || (slot->consumer && slot->consumer->Closed())) continue;
        // 每个订阅只进入一个索引桶，其余条件在投递前检查，避免重复发送。
        if (slot->filter.media_stream_id) routes->streams[slot->filter.media_stream_id].push_back(slot);
        else if (slot->filter.endpoint_id) routes->endpoints[slot->filter.endpoint_id].push_back(slot);
        else routes->all.push_back(slot);
    }
    return routes;
}

EncodedFrameRouter::SubscriptionId EncodedFrameRouter::Subscribe(std::shared_ptr<IEncodedFrameSink> sink)
{
    return Subscribe(std::move(sink), {});
}

EncodedFrameRouter::SubscriptionId EncodedFrameRouter::Subscribe(
    std::shared_ptr<IEncodedFrameSink> sink, EncodedFrameFilter filter)
{
    return SubscribeImpl(std::move(sink), std::move(filter), {});
}

EncodedFrameRouter::SubscriptionId EncodedFrameRouter::SubscribeImpl(
    std::shared_ptr<IEncodedFrameSink> sink, EncodedFrameFilter filter, MediaTrackSubscription::Ptr consumer)
{
    if (!sink || !filter.Valid() || (consumer && consumer->Closed())) return 0;
    std::lock_guard<std::mutex> lock(mutex_);
    if (!next_id_) return 0;
    auto next = slots_;
    for (auto it = next.begin(); it != next.end();)
        if (!it->second->active.load() || it->second->sink.expired() ||
            (it->second->consumer && it->second->consumer->Closed())) it = next.erase(it);
        else ++it;
    const auto id = next_id_;
    next.emplace(id, std::make_shared<Slot>(std::move(sink), std::move(filter), std::move(consumer)));
    auto routes = BuildRoutes(next);
    slots_.swap(next);
    std::atomic_store_explicit(&routes_, std::move(routes), std::memory_order_release);
    ++next_id_;
    return id;
}

void EncodedFrameRouter::Unsubscribe(SubscriptionId id)
{
    std::lock_guard<std::mutex> lock(mutex_);
    const auto found = slots_.find(id);
    if (found == slots_.end()) return;
    // 先封住已被 Publish 取得的旧快照，再发布新快照。
    found->second->active.store(false);
    if (found->second->consumer) found->second->consumer->Close();
    slots_.erase(found);
    try
    {
        auto routes = BuildRoutes(slots_);
        std::atomic_store_explicit(&routes_, std::move(routes), std::memory_order_release);
    }
    catch (const std::bad_alloc&)
    {
        // 内存不足也已退订；旧快照中的失效条目在下次控制操作时回收。
    }
}

size_t EncodedFrameRouter::Publish(const EncodedFrameEvent& event)
{
    return Dispatch(event).accepted;
}

EncodedFrameRouter::DispatchResult EncodedFrameRouter::Dispatch(const EncodedFrameEvent& event)
{
    DispatchResult result;
    if (!event.Valid()) return result;
    const auto routes = std::atomic_load_explicit(&routes_, std::memory_order_acquire);
    const auto deliver = [&](const auto& slots)
    {
        for (const auto& slot : slots)
        {
            if (!slot->active.load() || !slot->filter.Matches(event.source)) continue;
            if (slot->consumer && !slot->consumer->CanDeliver()) continue;
            const auto sink = slot->sink.lock();
            if (!sink) continue;
            try
            {
                if (sink->SubmitFrame(event)) ++result.accepted;
                else ++result.rejected;
            }
            catch (...) { ++result.failed; }
        }
    };
    deliver(routes->all);
    const auto stream = routes->streams.find(event.source.media_stream_id);
    if (stream != routes->streams.end()) deliver(stream->second);
    const auto endpoint = routes->endpoints.find(event.source.endpoint_id);
    if (endpoint != routes->endpoints.end()) deliver(endpoint->second);
    return result;
}
}
