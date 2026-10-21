#pragma once

#include "TranscodeTask.h"
#include "TranscodeRuntime.h"
#include "TranscodedFrame.h"
#include "EncodedFrameRouter.h"
#include "ThreadManager.h"

#include <condition_variable>
#include <deque>
#include <functional>
#include <mutex>

namespace service::transcode {

// Task lifecycle control must come from a control thread, never an output sink
// or keyframe callback. Status() is safe from either callback.
class TranscodeSession final : public media::IEncodedFrameSink,
                               public std::enable_shared_from_this<TranscodeSession>
{
public:
    TranscodeSession(std::shared_ptr<const TranscodeTask> task,
                     std::shared_ptr<media::EncodedFrameRouter> input,
                     std::shared_ptr<media::EncodedFrameRouter> output,
                     KeyFrameRequest request_keyframe = {}, VideoTranscoderFactory backend_factory = {});
    ~TranscodeSession() override;
    bool Start();
    void Stop();
    bool SubmitFrame(const media::EncodedFrameEvent& event) override;
    TranscodeTaskStatus Status() const;

private:
    struct QueuedFrame { media::EncodedFrameEvent event; bool reset = false; };
    bool Matches(const media::EncodedFrameEvent& event) const;
    void RecoverLocked(std::string reason);
    void FailLocked(std::string reason);
    void Run();
    bool Publish(const std::vector<TranscodedFrame>& frames);

    const std::shared_ptr<const TranscodeTask> task_;
    const TranscodeTaskConfig& config_;
    const std::shared_ptr<media::EncodedFrameRouter> input_, output_;
    const KeyFrameRequest request_keyframe_;
    const VideoTranscoderFactory backend_factory_;
    std::mutex lifecycle_mutex_;
    mutable std::mutex mutex_;
    std::condition_variable ready_;
    std::deque<QueuedFrame> queue_;
    TranscodeTaskStatus status_;
    bool stopping_ = false;
    bool reset_next_ = false;
    bool request_pending_ = true;
    utils::ManagedThread worker_;
    media::EncodedFrameRouter::SubscriptionId subscription_ = 0;
};

} // namespace service::transcode
