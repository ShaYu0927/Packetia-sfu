#ifndef PACKETIA_MEDIA_FRAME_ENCODED_FRAME_DISPATCHER_H_
#define PACKETIA_MEDIA_FRAME_ENCODED_FRAME_DISPATCHER_H_

#include "EncodedFrameRouter.h"

#include <atomic>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <unordered_map>

namespace media
{
// 拥有帧来源描述、回调和发布器；不依赖端点、RTP 接收器或订阅状态。
// Dispatch 在发布者线程执行，回调在锁外调用；调用者负责持有本对象的生命周期。
class EncodedFrameDispatcher
{
public:
    using Callback = std::function<void(const EncodedFrame::ConstPtr&)>;
    using SubscriptionId = uint64_t;

    EncodedFrameDispatcher(uint64_t endpoint_id, EncodedFrameSource source, std::shared_ptr<IEncodedFramePublisher> publisher);
    SubscriptionId AddCallback(Callback callback);
    void RemoveCallback(SubscriptionId id);
    void Dispatch(const EncodedFrame::Ptr& frame);

private:
    EncodedFrameSource source_;
    const std::shared_ptr<IEncodedFramePublisher> publisher_;
    std::mutex mutex_;
    std::unordered_map<SubscriptionId, Callback> callbacks_;
    std::atomic<SubscriptionId> next_id_{1};
    std::atomic<uint64_t> published_count_{0};
};
}
#endif
