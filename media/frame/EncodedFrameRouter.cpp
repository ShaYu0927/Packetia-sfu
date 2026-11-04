#include "EncodedFrameRouter.h"

#include <new>
#include <stdexcept>
#include <utility>

namespace media
{
EncodedFrameRouter::EncodedFrameRouter() : EncodedFrameRouter(Options{}) {}

EncodedFrameRouter::EncodedFrameRouter(Options options) : max_subscriptions_(options.max_subscriptions)
{
    if (!max_subscriptions_) throw std::invalid_argument("subscription limit must be positive");
}

EncodedFrameRouter::~EncodedFrameRouter()
{
    Close();
}

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
        if (slot->filter.media_stream_id && slot->filter.track_id)
            routes->tracks[TrackKey({slot->filter.media_stream_id, *slot->filter.track_id})].push_back(slot);
        else if (slot->filter.media_stream_id) routes->streams[slot->filter.media_stream_id].push_back(slot);
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
    return TrySubscribe(std::move(sink), std::move(filter)).id;
}

EncodedFrameRouter::SubscribeResult EncodedFrameRouter::TrySubscribe(std::shared_ptr<IEncodedFrameSink> sink, EncodedFrameFilter filter)
{
    return SubscribeImpl(std::move(sink), std::move(filter), {});
}

EncodedFrameRouter::SubscribeResult EncodedFrameRouter::SubscribeImpl(std::shared_ptr<IEncodedFrameSink> sink, EncodedFrameFilter filter, MediaTrackSubscription::Ptr consumer)
{
    if (!sink || !filter.Valid()) return {SubscribeStatus::Invalid, 0};
    if (consumer && consumer->Closed()) return {SubscribeStatus::SourceRejected, 0};
    // 新旧槽及旧快照都在写锁外析构，允许关闭回调的捕获对象重入控制接口。
    auto added = std::make_shared<Slot>(std::move(sink), std::move(filter), std::move(consumer));
    Slots next;
    std::shared_ptr<const Routes> retired_routes;
    std::lock_guard<std::mutex> lock(mutex_);
    if (closed_) return {SubscribeStatus::Closed, 0};
    if (!next_id_) return {SubscribeStatus::Exhausted, 0};
    next = slots_;
    for (auto it = next.begin(); it != next.end();)
        if (!it->second->active.load() || it->second->sink.expired() ||
            (it->second->consumer && it->second->consumer->Closed())) it = next.erase(it);
        else ++it;
    if (next.size() >= max_subscriptions_) return {SubscribeStatus::Full, 0};
    const auto id = next_id_;
    next.emplace(id, added);
    auto routes = BuildRoutes(next);
    for (const auto& entry : slots_)
        if (!next.count(entry.first))
        {
            entry.second->active.store(false);
            if (entry.second->consumer) entry.second->consumer->Close();
        }
    slots_.swap(next);
    retired_routes = std::atomic_exchange_explicit(&routes_, std::move(routes), std::memory_order_acq_rel);
    ++next_id_;
    return {SubscribeStatus::Accepted, id};
}

void EncodedFrameRouter::Unsubscribe(SubscriptionId id)
{
    std::shared_ptr<Slot> retired;
    std::shared_ptr<const Routes> retired_routes;
    std::lock_guard<std::mutex> lock(mutex_);
    const auto found = slots_.find(id);
    if (found == slots_.end()) return;
    // 先封住已被 Publish 取得的旧快照，再发布新快照。
    retired = found->second;
    retired->active.store(false);
    if (retired->consumer) retired->consumer->Close();
    slots_.erase(found);
    try
    {
        auto routes = BuildRoutes(slots_);
        retired_routes = std::atomic_exchange_explicit(&routes_, std::move(routes), std::memory_order_acq_rel);
    }
    catch (const std::bad_alloc&)
    {
        // 内存不足也已退订；旧快照中的失效条目在下次控制操作时回收。
    }
}

void EncodedFrameRouter::Close()
{
    Slots retired;
    std::shared_ptr<const Routes> retired_routes;
    std::lock_guard<std::mutex> lock(mutex_);
    if (closed_) return;
    closed_ = true;
    for (const auto& entry : slots_)
    {
        entry.second->active.store(false);
        if (entry.second->consumer) entry.second->consumer->Close();
    }
    retired.swap(slots_);
    // 空指针表示已关闭，避免停机时为新的空快照分配内存。
    retired_routes = std::atomic_exchange_explicit(&routes_, std::shared_ptr<const Routes>{},
        std::memory_order_acq_rel);
}

EncodedFrameRouter::Stats EncodedFrameRouter::GetStats() const
{
    Stats stats;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        stats.max_subscriptions = max_subscriptions_;
        stats.closed = closed_;
        for (const auto& entry : slots_)
        {
            const auto& slot = entry.second;
            if (slot->active.load() && !slot->sink.expired() && (!slot->consumer || !slot->consumer->Closed()))
                ++stats.active_subscriptions;
        }
    }
    stats.frames = frames_.load(std::memory_order_relaxed);
    stats.invalid_frames = invalid_frames_.load(std::memory_order_relaxed);
    stats.accepted = accepted_.load(std::memory_order_relaxed);
    stats.rejected = rejected_.load(std::memory_order_relaxed);
    stats.failed = failed_.load(std::memory_order_relaxed);
    stats.examined = examined_.load(std::memory_order_relaxed);
    return stats;
}

size_t EncodedFrameRouter::Publish(const EncodedFrameEvent& event)
{
    return Dispatch(event).accepted;
}

EncodedFrameRouter::DispatchResult EncodedFrameRouter::Dispatch(const EncodedFrameEvent& event)
{
    DispatchResult result;
    const auto routes = std::atomic_load_explicit(&routes_, std::memory_order_acquire);
    if (!routes) return result;
    if (!event.Valid())
    {
        invalid_frames_.fetch_add(1, std::memory_order_relaxed);
        return result;
    }
    frames_.fetch_add(1, std::memory_order_relaxed);
    const auto deliver = [&](const auto& slots)
    {
        for (const auto& slot : slots)
        {
            ++result.examined;
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
    const auto track = routes->tracks.find(TrackKey({event.source.media_stream_id, event.source.track_id}));
    if (track != routes->tracks.end()) deliver(track->second);
    const auto endpoint = routes->endpoints.find(event.source.endpoint_id);
    if (endpoint != routes->endpoints.end()) deliver(endpoint->second);
    accepted_.fetch_add(result.accepted, std::memory_order_relaxed);
    rejected_.fetch_add(result.rejected, std::memory_order_relaxed);
    failed_.fetch_add(result.failed, std::memory_order_relaxed);
    examined_.fetch_add(result.examined, std::memory_order_relaxed);
    return result;
}
}
