#pragma once

#include "IMixBackend.h"
#include "config/ConfigStore.h"

#include <condition_variable>
#include <deque>
#include <memory>
#include <mutex>
#include <thread>

namespace service::mix {

// One single-use conference task; create another object to restart/reconfigure.
// The control thread must retain shared ownership until Stop has returned.
class ConferenceMixer final : public media::IEncodedFrameSink,
                              public std::enable_shared_from_this<ConferenceMixer> {
public:
    ConferenceMixer(MixConfig config,
                    std::shared_ptr<media::EncodedFrameRouter> input,
                    std::shared_ptr<media::EncodedFrameRouter> output,
                    std::unique_ptr<IMixBackend> backend = {},
                    std::shared_ptr<const config::ConfigStore> policies = {});
    ~ConferenceMixer() override;

    ConferenceMixer(const ConferenceMixer&) = delete;
    ConferenceMixer& operator=(const ConferenceMixer&) = delete;

    // Call lifecycle methods from the application's control thread, never from
    // a backend/output-subscriber callback. Stop drains admitted frames and
    // joins the worker. No output is published after Stop returns.
    bool Start();
    void Stop();
    bool SubmitFrame(const media::EncodedFrameEvent& event) override;

    MixState State() const;
    MixStats Stats() const;
    std::string LastError() const;
    bool InputsEnabled() const;

private:
    bool Validate(std::string& error) const;
    bool Matches(const media::EncodedFrameEvent& event) const;
    bool PublishOutput(const media::EncodedFrame::ConstPtr& frame);
    void Run();
    void FailLocked(std::string error);

    const MixConfig config_;
    std::shared_ptr<const config::ConfigStore> policies_;
    std::shared_ptr<media::EncodedFrameRouter> input_;
    std::shared_ptr<media::EncodedFrameRouter> output_;
    std::unique_ptr<IMixBackend> backend_;
    std::mutex lifecycle_mutex_;
    mutable std::mutex mutex_;
    std::condition_variable ready_;
    std::deque<media::EncodedFrameEvent> queue_;
    std::thread worker_;
    media::EncodedFrameRouter::SubscriptionId subscription_ = 0;
    MixState state_ = MixState::Created;
    MixStats stats_;
    std::string error_;
    bool stop_requested_ = false;
};

} // namespace service::mix
