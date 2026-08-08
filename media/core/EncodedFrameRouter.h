#ifndef _ENCODED_FRAME_ROUTER_H_
#define _ENCODED_FRAME_ROUTER_H_

#include "../MediaFrame/MediaFrame.h"

#include <atomic>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

namespace media
{

struct EncodedFrameSource
{
    uint64_t endpoint_id = 0;
    std::string session_id;
    std::string stream_id;
    TrackId track_id = 0;
    uint32_t ssrc = 0;
};

struct EncodedFrameEvent
{
    EncodedFrameSource source;
    EncodedFrame::ConstPtr frame;

    bool Valid() const noexcept
    {
        return source.endpoint_id != 0 && frame && frame->Valid();
    }
};

class IEncodedFrameSink
{
public:
    virtual ~IEncodedFrameSink() = default;

    // This method executes on a media worker and must never block.
    virtual bool TryEnqueue(const EncodedFrameEvent& event) = 0;
};

class IEncodedFramePublisher
{
public:
    virtual ~IEncodedFramePublisher() = default;
    virtual size_t Publish(const EncodedFrameEvent& event) = 0;
};

class EncodedFrameRouter final : public IEncodedFramePublisher
{
public:
    using SubscriptionId = uint64_t;

    SubscriptionId Subscribe(std::shared_ptr<IEncodedFrameSink> sink);
    void Unsubscribe(SubscriptionId id);
    size_t Publish(const EncodedFrameEvent& event) override;

private:
    struct Slot
    {
        explicit Slot(std::shared_ptr<IEncodedFrameSink> value)
            : sink(std::move(value))
        {
        }

        std::atomic<bool> active{true};
        std::weak_ptr<IEncodedFrameSink> sink;
    };

    std::mutex mutex_;
    std::unordered_map<SubscriptionId, std::shared_ptr<Slot>> slots_;
    std::atomic<SubscriptionId> next_id_{1};
};

} // namespace media

#endif /* _ENCODED_FRAME_ROUTER_H_ */
