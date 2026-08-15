#include "EncodedFrameRouter.h"
#include "logger.h"

#include <utility>

namespace media
{

EncodedFrameRouter::SubscriptionId EncodedFrameRouter::Subscribe(
    std::shared_ptr<IEncodedFrameSink> sink)
{
    if (!sink)
    {
        return 0;
    }

    const SubscriptionId id = next_id_.fetch_add(1);
    auto slot = std::make_shared<Slot>(std::move(sink));
    std::lock_guard<std::mutex> lock(mutex_);
    slots_.emplace(id, std::move(slot));
    LOG_INFO("[FRAME_ROUTER] sink subscribed, subscription_id=", id,
             ", sink_count=", slots_.size());
    return id;
}

void EncodedFrameRouter::Unsubscribe(SubscriptionId id)
{
    std::shared_ptr<Slot> removed;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        const auto it = slots_.find(id);
        if (it == slots_.end())
        {
            return;
        }
        removed = std::move(it->second);
        slots_.erase(it);
        LOG_INFO("[FRAME_ROUTER] sink unsubscribed, subscription_id=", id,
                 ", sink_count=", slots_.size());
    }
    removed->active.store(false);
}

size_t EncodedFrameRouter::Publish(const EncodedFrameEvent& event)
{
    if (!event.Valid())
    {
        return 0;
    }

    std::vector<std::shared_ptr<Slot>> slots;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        slots.reserve(slots_.size());
        for (const auto& entry : slots_)
        {
            slots.push_back(entry.second);
        }
    }

    size_t accepted = 0;
    for (const auto& slot : slots)
    {
        if (!slot->active.load())
        {
            continue;
        }

        const auto sink = slot->sink.lock();
        if (sink && sink->TryEnqueue(event))
        {
            ++accepted;
        }
    }

    return accepted;
}

} // namespace media
