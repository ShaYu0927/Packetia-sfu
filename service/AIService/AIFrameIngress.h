#ifndef _AI_FRAME_INGRESS_H_
#define _AI_FRAME_INGRESS_H_

#include "EncodedFrameRouter.h"

#include <atomic>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <memory>
#include <mutex>
#include <thread>

namespace service
{
namespace ai
{

class IAIFrameProcessor
{
public:
    virtual ~IAIFrameProcessor() = default;
    virtual void Process(const media::EncodedFrameEvent& event) = 0;
};

struct AIFrameIngressStats
{
    uint64_t accepted = 0;
    uint64_t processed = 0;
    uint64_t dropped = 0;
    size_t queue_depth = 0;
};

class AIFrameIngress final : public media::IEncodedFrameSink,
                             public std::enable_shared_from_this<AIFrameIngress>
{
public:
    explicit AIFrameIngress(std::shared_ptr<media::EncodedFrameRouter> router,
                            std::shared_ptr<IAIFrameProcessor> processor,
                            size_t max_queue_size = 128);
    ~AIFrameIngress();

    bool Start();
    void Stop();
    AIFrameIngressStats Stats() const;
    bool TryEnqueue(const media::EncodedFrameEvent& event) override;

private:
    void Run();

    const size_t max_queue_size_;
    std::shared_ptr<media::EncodedFrameRouter> router_;
    std::shared_ptr<IAIFrameProcessor> processor_;
    mutable std::mutex mutex_;
    std::condition_variable ready_;
    std::deque<media::EncodedFrameEvent> queue_;
    std::thread worker_;
    bool running_ = false;
    media::EncodedFrameRouter::SubscriptionId subscription_id_ = 0;
    std::atomic<uint64_t> accepted_{0};
    std::atomic<uint64_t> processed_{0};
    std::atomic<uint64_t> dropped_{0};
};

} // namespace ai
} // namespace service

#endif /* _AI_FRAME_INGRESS_H_ */
