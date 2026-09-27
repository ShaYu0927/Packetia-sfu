#include "ConferenceMixer.h"

#include <algorithm>
#include <chrono>
#include <exception>
#include <set>
#include <utility>

namespace service::mix {

ConferenceMixer::ConferenceMixer(MixConfig config,
    std::shared_ptr<media::EncodedFrameRouter> input,
    std::shared_ptr<media::EncodedFrameRouter> output,
    std::unique_ptr<IMixBackend> backend, std::shared_ptr<const config::ConfigStore> policies)
    : config_(std::move(config)), policies_(std::move(policies)), input_(std::move(input)),
      output_(std::move(output)), backend_(std::move(backend)) {}

ConferenceMixer::~ConferenceMixer() { Stop(); }

bool ConferenceMixer::Validate(std::string& error) const {
    if (!backend_) error = "mix backend is not implemented/configured";
    else if (!input_ || !output_ || input_ == output_)
        error = "input and output require distinct frame routers";
    else if (config_.room_id.empty() || config_.output_stream_id.empty() ||
             !config_.output_endpoint_id || config_.inputs.empty())
        error = "room, output identity and input tracks are required";
    else if (!config_.max_queue_frames || !config_.max_queue_bytes || !config_.tick_interval_ms)
        error = "queue limits and tick interval must be positive";
    else if (!InputsEnabled()) error = "conference input disabled or missing stream configuration identity";
    else {
        std::set<std::pair<uint64_t, media::TrackId>> keys;
        for (const auto& input : config_.inputs) {
            if (input.participant_id.empty() || !input.endpoint_id ||
                input.endpoint_id == config_.output_endpoint_id ||
                !keys.emplace(input.endpoint_id, input.track_id).second) {
                error = "input identities must be unique and distinct from the output endpoint";
                break;
            }
        }
    }
    return error.empty();
}

bool ConferenceMixer::Start() {
    std::lock_guard<std::mutex> lifecycle(lifecycle_mutex_);
    auto self = weak_from_this().lock();
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (state_ == MixState::Running) return true;
        if (state_ != MixState::Created) return false;
        std::string error;
        if (!self || !Validate(error)) {
            FailLocked(self ? std::move(error) : "ConferenceMixer requires shared ownership");
            return false;
        }
        state_ = MixState::Starting;
    }
    try {
        subscription_ = input_->Subscribe(self);
        if (!subscription_) {
            std::lock_guard<std::mutex> lock(mutex_);
            FailLocked("failed to subscribe to input frames");
            return false;
        }
        worker_ = std::thread(&ConferenceMixer::Run, this);
    } catch (const std::exception& error) {
        input_->Unsubscribe(subscription_);
        subscription_ = 0;
        std::lock_guard<std::mutex> lock(mutex_);
        FailLocked(error.what());
        return false;
    }
    bool started;
    {
        std::unique_lock<std::mutex> lock(mutex_);
        ready_.wait(lock, [this] { return state_ != MixState::Starting; });
        started = state_ == MixState::Running;
    }
    if (!started) {
        input_->Unsubscribe(subscription_);
        subscription_ = 0;
        worker_.join();
    }
    return started;
}

void ConferenceMixer::Stop() {
    std::lock_guard<std::mutex> lifecycle(lifecycle_mutex_);
    {
        std::lock_guard<std::mutex> lock(mutex_);
        stop_requested_ = true;
        if (state_ == MixState::Created) state_ = MixState::Stopped;
        else if (state_ == MixState::Running) state_ = MixState::Stopping;
    }
    if (input_ && subscription_) input_->Unsubscribe(subscription_);
    subscription_ = 0;
    ready_.notify_all();
    if (worker_.joinable()) worker_.join();
}

bool ConferenceMixer::Matches(const media::EncodedFrameEvent& event) const {
    return std::any_of(config_.inputs.begin(), config_.inputs.end(), [&](const MixInput& input) {
        return input.endpoint_id == event.source.endpoint_id && input.track_id == event.source.track_id &&
            (input.session_id.empty() || input.session_id == event.source.session_id) &&
            (input.stream_id.empty() || input.stream_id == event.source.stream_id);
    });
}

bool ConferenceMixer::SubmitFrame(const media::EncodedFrameEvent& event) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (state_ != MixState::Running || !event.Valid() || !event.frame->IsComplete() ||
        event.source.track_id != event.frame->info.track_id || !Matches(event) ||
        (policies_ && !policies_->Allows(config::Feature::ConferenceMix,
            {event.source.session_id, event.source.stream_id}))) {
        ++stats_.rejected;
        return false;
    }
    const auto bytes = event.frame->StorageSize();
    if (queue_.size() >= config_.max_queue_frames ||
        bytes > config_.max_queue_bytes - stats_.queue_bytes) {
        ++stats_.rejected;
        // Arbitrarily dropping encoded frames can corrupt decoder references.
        // Until recovery policy is implemented, fail the task explicitly.
        FailLocked("input queue capacity exceeded; task stopped to preserve decoder consistency");
        return false;
    }
    queue_.push_back(event);
    ++stats_.accepted;
    stats_.queue_frames = queue_.size();
    stats_.queue_bytes += bytes;
    ready_.notify_one();
    return true;
}

bool ConferenceMixer::PublishOutput(const media::EncodedFrame::ConstPtr& frame) {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        const bool video = frame && frame->info.media_type == media::MediaType::Video &&
                           frame->info.codec == media::CodecType::H264;
        const bool audio = frame && frame->info.media_type == media::MediaType::Audio &&
                           frame->info.codec == media::CodecType::AAC;
        if ((state_ != MixState::Running && state_ != MixState::Stopping) ||
            !frame || !frame->Valid() || !frame->IsComplete() || (!video && !audio)) {
            ++stats_.output_rejected;
            return false;
        }
    }
    try {
        media::EncodedFrameEvent event;
        event.source.endpoint_id = config_.output_endpoint_id;
        event.source.session_id = config_.room_id;
        event.source.stream_id = config_.output_stream_id;
        event.source.track_id = frame->info.track_id;
        event.source.ssrc = frame->rtp.ssrc;
        event.frame = frame;
        const auto accepted = output_->Publish(event);
        std::lock_guard<std::mutex> lock(mutex_);
        ++stats_.published;
        return accepted != 0;
    } catch (...) {
        // In particular, do not propagate sink exceptions into noexcept Stop.
        std::lock_guard<std::mutex> lock(mutex_);
        ++stats_.output_rejected;
        FailLocked("output frame delivery failed");
        return false;
    }
}

void ConferenceMixer::Run() {
    using Clock = std::chrono::steady_clock;
    try {
        if (!backend_->Start(config_, [this](const media::EncodedFrame::ConstPtr& frame) {
                return PublishOutput(frame);
            })) {
            std::lock_guard<std::mutex> lock(mutex_);
            FailLocked("mix backend initialization failed");
        } else {
            {
                std::lock_guard<std::mutex> lock(mutex_);
                state_ = MixState::Running;
                ready_.notify_all();
            }
            const auto origin = Clock::now();
            auto next_tick = origin;
            while (true) {
                media::EncodedFrameEvent event;
                {
                    std::unique_lock<std::mutex> lock(mutex_);
                    ready_.wait_until(lock, next_tick, [this] {
                        return stop_requested_ || !queue_.empty();
                    });
                    if (stop_requested_ && queue_.empty()) break;
                    if (!queue_.empty()) {
                        event = std::move(queue_.front());
                        queue_.pop_front();
                        stats_.queue_frames = queue_.size();
                        stats_.queue_bytes -= event.frame->StorageSize();
                    }
                }
                if (event.frame) {
                    backend_->InputFrame(event);
                    std::lock_guard<std::mutex> lock(mutex_);
                    ++stats_.processed;
                }
                {
                    std::lock_guard<std::mutex> lock(mutex_);
                    if (state_ == MixState::Failed) break;
                }
                const auto now = Clock::now();
                if (now >= next_tick) {
                    backend_->Tick(std::chrono::duration_cast<std::chrono::microseconds>(now - origin).count());
                    next_tick = now + std::chrono::milliseconds(config_.tick_interval_ms);
                }
            }
        }
    } catch (const std::exception& error) {
        std::lock_guard<std::mutex> lock(mutex_);
        FailLocked(error.what());
    } catch (...) {
        std::lock_guard<std::mutex> lock(mutex_);
        FailLocked("mix backend threw an unknown exception");
    }
    backend_->Stop();
    std::lock_guard<std::mutex> lock(mutex_);
    if (state_ != MixState::Failed) state_ = MixState::Stopped;
    ready_.notify_all();
}

void ConferenceMixer::FailLocked(std::string error) {
    error_ = std::move(error);
    state_ = MixState::Failed;
    stop_requested_ = true;
    stats_.discarded += queue_.size();
    queue_.clear();
    stats_.queue_frames = stats_.queue_bytes = 0;
    ready_.notify_all();
}

MixState ConferenceMixer::State() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return state_;
}

MixStats ConferenceMixer::Stats() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return stats_;
}

std::string ConferenceMixer::LastError() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return error_;
}

bool ConferenceMixer::InputsEnabled() const {
    if (!policies_) return true;
    const auto snapshot = policies_->Snapshot();
    for (const auto& input : config_.inputs)
        if (input.session_id.empty() || input.stream_id.empty() ||
            !snapshot->Resolve({input.session_id, input.stream_id}).conference_mix) return false;
    return snapshot->conference_mix_enabled;
}

} // namespace service::mix
