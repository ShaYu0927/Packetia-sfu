#include "TranscodeSession.h"
#include "VideoTranscoder.h"

#include <algorithm>
#include <exception>
#include <stdexcept>
#include <utility>

namespace service::transcode {

TranscodeSession::TranscodeSession(std::shared_ptr<const TranscodeTask> task,
    std::shared_ptr<media::EncodedFrameRouter> input,
    std::shared_ptr<media::EncodedFrameRouter> output, KeyFrameRequest request_keyframe,
    VideoTranscoderFactory backend_factory)
    : task_(std::move(task)),
      config_(task_ ? task_->Config() : throw std::invalid_argument("transcode task is required")),
      input_(std::move(input)), output_(std::move(output)), request_keyframe_(std::move(request_keyframe)),
      backend_factory_(std::move(backend_factory))
{
    status_.task_id = config_.task_id;
}

TranscodeSession::~TranscodeSession() { Stop(); }

bool TranscodeSession::Start()
{
    std::lock_guard<std::mutex> lifecycle(lifecycle_mutex_);
    const auto self = weak_from_this().lock();
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (status_.state == TranscodeTaskState::Running) return true;
        if (status_.state != TranscodeTaskState::Created) return false;
        if (!self || !input_ || !output_ || input_ == output_) {
            FailLocked("execution session requires shared ownership and distinct frame routers");
            return false;
        }
        std::string error;
        if (!task_->Validate(error)) { FailLocked(std::move(error)); return false; }
        status_.state = TranscodeTaskState::Starting;
    }
    try {
        subscription_ = input_->Subscribe(self);
        if (!subscription_) throw std::runtime_error("could not subscribe to input frames");
        worker_ = utils::ThreadManager::Instance().Start(
            {"transcode", "transcode-" + config_.task_id, utils::ThreadModel::Dedicated},
            [this] { Run(); }, [this] {
                { std::lock_guard<std::mutex> lock(mutex_); stopping_ = true; }
                ready_.notify_all();
            });
        bool running;
        {
            std::unique_lock<std::mutex> lock(mutex_);
            ready_.wait(lock, [this] { return status_.state != TranscodeTaskState::Starting; });
            running = status_.state == TranscodeTaskState::Running;
        }
        if (running) return true;
    } catch (const std::exception& error) {
        std::lock_guard<std::mutex> lock(mutex_);
        FailLocked(error.what());
    }
    if (subscription_) input_->Unsubscribe(subscription_);
    subscription_ = 0;
    worker_.RequestStop();
    worker_.Join();
    return false;
}

void TranscodeSession::Stop()
{
    std::lock_guard<std::mutex> lifecycle(lifecycle_mutex_);
    {
        std::lock_guard<std::mutex> lock(mutex_);
        stopping_ = true;
        if (status_.state == TranscodeTaskState::Running) status_.state = TranscodeTaskState::Stopping;
        if (status_.state == TranscodeTaskState::Created) status_.state = TranscodeTaskState::Stopped;
    }
    if (input_ && subscription_) input_->Unsubscribe(subscription_);
    subscription_ = 0;
    ready_.notify_all();
    worker_.RequestStop();
    worker_.Join();
}

bool TranscodeSession::Matches(const media::EncodedFrameEvent& event) const
{
    const auto& source = event.source;
    return config_.input.Matches({source.endpoint_id, source.track_id, source.session_id, source.stream_id, source.ssrc});
}

void TranscodeSession::RecoverLocked(std::string reason)
{
    status_.last_error = std::move(reason);
    status_.stats.dropped += queue_.size();
    queue_.clear();
    status_.stats.queue_frames = status_.stats.queue_bytes = 0;
    status_.waiting_for_idr = true;
    reset_next_ = true;
    request_pending_ = true;
    ++status_.stats.recoveries;
    ready_.notify_one();
}

void TranscodeSession::FailLocked(std::string reason)
{
    status_.last_error = std::move(reason);
    status_.state = TranscodeTaskState::Failed;
    stopping_ = true;
    ++status_.stats.errors;
    status_.stats.dropped += queue_.size();
    queue_.clear();
    status_.stats.queue_frames = status_.stats.queue_bytes = 0;
    ready_.notify_all();
}

bool TranscodeSession::SubmitFrame(const media::EncodedFrameEvent& event)
{
    std::lock_guard<std::mutex> lock(mutex_);
    if (status_.state != TranscodeTaskState::Running || stopping_ || !event.Valid() || !Matches(event) ||
        event.source.track_id != event.frame->info.track_id ||
        event.frame->info.media_type != media::MediaType::Video ||
        event.frame->info.codec != config_.video.input_codec) return false;
    if (!event.frame->IsComplete()) {
        ++status_.stats.dropped;
        RecoverLocked("incomplete video input; waiting for a key frame");
        return false;
    }
    const auto bytes = event.frame->StorageSize();
    if (queue_.size() >= config_.queue.max_frames || bytes > config_.queue.max_bytes - status_.stats.queue_bytes)
        RecoverLocked("input queue capacity exceeded; waiting for IDR");
    if ((status_.waiting_for_idr && !event.frame->IsKeyFrame()) || bytes > config_.queue.max_bytes) {
        ++status_.stats.dropped;
        return false;
    }
    queue_.push_back({event, reset_next_});
    reset_next_ = false;
    status_.waiting_for_idr = false;
    status_.stats.queue_frames = queue_.size();
    status_.stats.queue_bytes += bytes;
    ++status_.stats.accepted;
    ready_.notify_one();
    return true;
}

bool TranscodeSession::Publish(const std::vector<TranscodedFrame>& frames)
{
    for (const auto& frame : frames) {
        media::EncodedFrameEvent event;
        event.source.endpoint_id = config_.output.endpoint_id;
        event.source.session_id = config_.output.session_id;
        event.source.stream_id = config_.output.StreamFor(frame.rendition_id);
        event.source.track_id = frame.frame->info.track_id;
        event.frame = frame.frame;
        const auto accepted = output_->Publish(event);
        std::lock_guard<std::mutex> lock(mutex_);
        ++status_.stats.published;
        if (!accepted) ++status_.stats.output_unaccepted;
    }
    return true;
}

void TranscodeSession::Run()
{
    auto config = config_.video;
    // Keep the RTP epoch outside the codec, so decoder recovery cannot reset it.
    config.timestamp_mode = InputTimestampMode::MediaTime;
    bool timestamp_started = false;
    int64_t highest_timestamp = 0;
    uint32_t source_ssrc = config_.input.ssrc;
    std::unique_ptr<IVideoTranscoder> transcoder;
    auto update_stats = [&](VideoTranscodeStats before) {
        const auto after = transcoder->Stats();
        std::lock_guard<std::mutex> lock(mutex_);
        status_.stats.decoded_frames += after.decoded_frames - before.decoded_frames;
        status_.stats.encoded_frames += after.encoded_frames - before.encoded_frames;
    };
    try {
        transcoder = backend_factory_ ? backend_factory_() : std::make_unique<VideoTranscoder>();
        if (!transcoder) {
            std::lock_guard<std::mutex> lock(mutex_);
            FailLocked("transcode backend factory returned no backend");
            return;
        }
        if (!transcoder->Open(config)) {
            std::lock_guard<std::mutex> lock(mutex_);
            FailLocked(transcoder->LastError());
            return;
        }
        {
            std::lock_guard<std::mutex> lock(mutex_);
            status_.state = TranscodeTaskState::Running;
            ready_.notify_all();
        }
        while (true) {
            QueuedFrame item;
            bool request = false;
            {
                std::unique_lock<std::mutex> lock(mutex_);
                ready_.wait(lock, [this] { return stopping_ || request_pending_ || !queue_.empty(); });
                if (status_.state == TranscodeTaskState::Failed) break;
                if (stopping_ && queue_.empty()) break;
                request = std::exchange(request_pending_, false) && !stopping_;
                if (!queue_.empty()) {
                    item = std::move(queue_.front());
                    queue_.pop_front();
                    status_.stats.queue_frames = queue_.size();
                    status_.stats.queue_bytes -= item.event.frame->StorageSize();
                }
            }
            if (request && request_keyframe_) request_keyframe_();
            if (!item.event.frame) continue;
            if (item.reset && !transcoder->Open(config)) {
                std::lock_guard<std::mutex> lock(mutex_);
                FailLocked(transcoder->LastError());
                break;
            }
            const auto& incoming = item.event;
            // A new source timeline requires an explicit task restart, rather
            // than mixing unrelated epochs/reference pictures into this task.
            if (source_ssrc && incoming.source.ssrc && source_ssrc != incoming.source.ssrc) {
                std::lock_guard<std::mutex> lock(mutex_);
                FailLocked("input SSRC changed; restart the transcode task for the new source");
                break;
            }
            if (incoming.source.ssrc) source_ssrc = incoming.source.ssrc;
            media::EncodedFrame frame = *incoming.frame;
            if (config_.video.timestamp_mode == InputTimestampMode::Rtp32) {
                auto& ts = frame.info.timestamp;
                if (ts.time_base_num != 1 || ts.time_base_den != 90000 || ts.pts < 0 || ts.pts > UINT32_MAX) {
                    std::lock_guard<std::mutex> lock(mutex_);
                    FailLocked("Rtp32 task requires raw uint32 PTS in 1/90000 units");
                    break;
                }
                int64_t pts = ts.pts;
                if (timestamp_started) {
                    int64_t delta = uint32_t(uint32_t(pts) - uint32_t(highest_timestamp));
                    if (delta >= INT64_C(0x80000000)) delta -= INT64_C(0x100000000);
                    if (delta > 0 && highest_timestamp > INT64_MAX - delta) {
                        std::lock_guard<std::mutex> lock(mutex_);
                        FailLocked("extended RTP timestamp overflow");
                        break;
                    }
                    pts = highest_timestamp + delta;
                }
                highest_timestamp = timestamp_started ? std::max(pts, highest_timestamp) : pts;
                timestamp_started = true;
                ts.pts = ts.dts = pts;
            }
            std::vector<TranscodedFrame> output;
            const auto before = transcoder->Stats();
            const bool ok = transcoder->Input(frame, output);
            update_stats(before);
            if (!ok) {
                std::lock_guard<std::mutex> lock(mutex_);
                ++status_.stats.errors;
                ++status_.stats.dropped;
                RecoverLocked(transcoder->LastError());
                continue;
            }
            Publish(output);
            std::lock_guard<std::mutex> lock(mutex_);
            ++status_.stats.processed;
        }
        bool flush;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            // A failed decoder or a recovery gap cannot be flushed as a
            // healthy stream. Accepted work on normal stop is fully drained.
            flush = status_.state != TranscodeTaskState::Failed && !reset_next_;
        }
        if (flush) {
            std::vector<TranscodedFrame> output;
            const auto before = transcoder->Stats();
            const bool ok = transcoder->Flush(output);
            update_stats(before);
            if (ok) Publish(output);
            else {
                std::lock_guard<std::mutex> lock(mutex_);
                FailLocked(transcoder->LastError());
            }
        }
    } catch (const std::exception& error) {
        std::lock_guard<std::mutex> lock(mutex_);
        FailLocked(error.what());
    } catch (...) {
        std::lock_guard<std::mutex> lock(mutex_);
        FailLocked("unknown transcode worker error");
    }
    std::lock_guard<std::mutex> lock(mutex_);
    if (status_.state != TranscodeTaskState::Failed) status_.state = TranscodeTaskState::Stopped;
    ready_.notify_all();
}

TranscodeTaskStatus TranscodeSession::Status() const
{
    std::lock_guard<std::mutex> lock(mutex_);
    return status_;
}

} // namespace service::transcode
