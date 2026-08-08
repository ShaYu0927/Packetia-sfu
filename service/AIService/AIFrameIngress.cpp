#include "AIFrameIngress.h"
#include "logger.h"

#include <utility>

namespace service
{
namespace ai
{

AIFrameIngress::AIFrameIngress(std::shared_ptr<media::EncodedFrameRouter> router,
                               std::shared_ptr<IAIFrameProcessor> processor,
                               size_t max_queue_size)
    : max_queue_size_(max_queue_size == 0 ? 1 : max_queue_size),
      router_(std::move(router)),
      processor_(std::move(processor))
{
}

AIFrameIngress::~AIFrameIngress()
{
    Stop();
}

bool AIFrameIngress::Start()
{
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (running_)
        {
            return true;
        }
        running_ = true;
    }

    if (!router_)
    {
        std::lock_guard<std::mutex> lock(mutex_);
        running_ = false;
        return false;
    }

    const auto subscription_id = router_->Subscribe(shared_from_this());
    if (subscription_id == 0)
    {
        std::lock_guard<std::mutex> lock(mutex_);
        running_ = false;
        return false;
    }

    {
        std::lock_guard<std::mutex> lock(mutex_);
        subscription_id_ = subscription_id;
    }

    try
    {
        worker_ = std::thread(&AIFrameIngress::Run, this);
    }
    catch (...)
    {
        router_->Unsubscribe(subscription_id);
        std::lock_guard<std::mutex> lock(mutex_);
        subscription_id_ = 0;
        running_ = false;
        return false;
    }
    LOG_INFO("[AI_FRAME] ingress started, subscription_id=", subscription_id,
             ", queue_capacity=", max_queue_size_);
    return true;
}

void AIFrameIngress::Stop()
{
    media::EncodedFrameRouter::SubscriptionId subscription_id = 0;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        subscription_id = subscription_id_;
        subscription_id_ = 0;
        running_ = false;
    }

    if (router_)
    {
        router_->Unsubscribe(subscription_id);
    }
    ready_.notify_all();
    if (worker_.joinable())
    {
        worker_.join();
    }

    std::lock_guard<std::mutex> lock(mutex_);
    dropped_.fetch_add(queue_.size());
    queue_.clear();
    LOG_INFO("[AI_FRAME] ingress stopped, accepted=", accepted_.load(),
             ", processed=", processed_.load(),
             ", dropped=", dropped_.load());
}

AIFrameIngressStats AIFrameIngress::Stats() const
{
    AIFrameIngressStats stats;
    stats.accepted = accepted_.load();
    stats.processed = processed_.load();
    stats.dropped = dropped_.load();
    std::lock_guard<std::mutex> lock(mutex_);
    stats.queue_depth = queue_.size();
    return stats;
}

bool AIFrameIngress::TryEnqueue(const media::EncodedFrameEvent& event)
{
    std::lock_guard<std::mutex> lock(mutex_);
    if (!running_ || !event.Valid())
    {
        return false;
    }

    if (queue_.size() >= max_queue_size_)
    {
        queue_.pop_front();
        const uint64_t dropped = dropped_.fetch_add(1) + 1;
        if (dropped == 1 || dropped % 100 == 0)
        {
            LOG_ERROR("[AI_FRAME] queue overflow, dropped=", dropped,
                      ", endpoint_id=", event.source.endpoint_id,
                      ", stream_id=", event.source.stream_id,
                      ", track_id=", event.source.track_id,
                      ", queue_capacity=", max_queue_size_);
        }
    }
    queue_.push_back(event);
    accepted_.fetch_add(1);
    const uint64_t accepted = accepted_.load();
    if (accepted == 1 || accepted % 300 == 0)
    {
        LOG_INFO("[AI_FRAME] frame enqueued, accepted=", accepted,
                 ", endpoint_id=", event.source.endpoint_id,
                 ", session_id=", event.source.session_id,
                 ", stream_id=", event.source.stream_id,
                 ", track_id=", event.source.track_id,
                 ", ssrc=", event.source.ssrc,
                 ", queue_depth=", queue_.size());
    }
    ready_.notify_one();
    return true;
}

void AIFrameIngress::Run()
{
    while (true)
    {
        media::EncodedFrameEvent event;
        {
            std::unique_lock<std::mutex> lock(mutex_);
            ready_.wait(lock, [this]() { return !running_ || !queue_.empty(); });
            if (!running_)
            {
                break;
            }
            event = std::move(queue_.front());
            queue_.pop_front();
        }

        if (processor_)
        {
            try
            {
                processor_->Process(event);
            }
            catch (...)
            {
                // A processor failure must never escape into the media path.
            }
        }
        const uint64_t processed = processed_.fetch_add(1) + 1;
        if (processed == 1 || processed % 300 == 0)
        {
            LOG_INFO("[AI_FRAME] frame consumed, processed=", processed,
                     ", endpoint_id=", event.source.endpoint_id,
                     ", stream_id=", event.source.stream_id,
                     ", track_id=", event.source.track_id,
                     ", has_processor=", processor_ != nullptr);
        }
    }
}

} // namespace ai
} // namespace service
