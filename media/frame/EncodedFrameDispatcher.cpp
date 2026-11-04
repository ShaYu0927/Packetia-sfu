#include "EncodedFrameDispatcher.h"
#include "logger.h"

#include <utility>
#include <vector>

namespace media
{
EncodedFrameDispatcher::EncodedFrameDispatcher(uint64_t endpoint_id, EncodedFrameSource source,
    std::shared_ptr<IEncodedFramePublisher> publisher)
    : source_(std::move(source)),
      publisher_(std::move(publisher))
{
    source_.endpoint_id = endpoint_id;
}

EncodedFrameDispatcher::SubscriptionId EncodedFrameDispatcher::AddCallback(Callback cb)
{
    if (!cb)
    {
        return 0;
    }

    const SubscriptionId id = next_id_.fetch_add(1);
    std::lock_guard<std::mutex> lock(mutex_);
    callbacks_[id] = std::move(cb);
    return id;
}

void EncodedFrameDispatcher::RemoveCallback(SubscriptionId id)
{
    Callback retired;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        const auto found = callbacks_.find(id);
        if (found == callbacks_.end()) return;
        retired = std::move(found->second);
        callbacks_.erase(found);
    }
}

void EncodedFrameDispatcher::Dispatch(const media::EncodedFrame::Ptr& frame)
{
    if (!frame || !frame->Valid())
    {
        return;
    }

    std::vector<Callback> callbacks;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        callbacks.reserve(callbacks_.size());
        for (const auto& entry : callbacks_)
        {
            callbacks.push_back(entry.second);
        }
    }

    const media::EncodedFrame::ConstPtr immutable_frame = frame;
    if (publisher_)
    {
        EncodedFrameEvent event;
        event.source = source_;
        event.source.track_id = frame->info.track_id;
        event.source.ssrc = frame->rtp.ssrc;
        event.frame = immutable_frame;
        const size_t accepted = publisher_->Publish(event);
        const uint64_t count = published_count_.fetch_add(1) + 1;
        if (count == 1)
        {
            LOG_INFO("[FRAME_SOURCE] encoded frame published, count=", count,
                     ", endpoint_id=", event.source.endpoint_id,
                     ", session_id=", event.source.session_id,
                     ", stream_id=", event.source.stream_id,
                     ", track_id=", event.source.track_id,
                     ", ssrc=", event.source.ssrc,
                     ", media_type=", static_cast<int>(frame->info.media_type),
                     ", codec=", static_cast<int>(frame->info.codec),
                     ", bytes=", frame->Size(),
                     ", accepted_sinks=", accepted);
        }
        else if (count % 300 == 0)
        {
            LOG_DEBUG("[FRAME_SOURCE] encoded frame published, count=", count,
                      ", endpoint_id=", event.source.endpoint_id,
                      ", track_id=", event.source.track_id,
                      ", bytes=", frame->Size(),
                      ", accepted_sinks=", accepted);
        }
    }
    for (const auto& callback : callbacks)
    {
        callback(immutable_frame);
    }
}

}
