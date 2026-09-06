#include "RecordingService.h"
#include "Mp4Recorder.h"
#include "RecordingDispatcher.h"
#include "logger.h"
#include <chrono>
#include <filesystem>
#include <stdexcept>
#include <utility>

namespace service {
RecordingService::RecordingService(std::shared_ptr<media::EncodedFrameRouter> router, RecordingOptions options)
    : router_(std::move(router)), options_(std::move(options)) {}
RecordingService::~RecordingService() { Stop(); }

bool RecordingService::Init()
{
    std::lock_guard<std::mutex> lifecycle(lifecycle_mutex_);
    if (State() == ServiceState::Running) return true;
    std::error_code error;
    std::filesystem::create_directories(options_.directory, error);
    if (!router_ || error || !options_.max_queue_frames || !options_.max_queue_bytes ||
        !options_.max_streams || !options_.max_pending_bytes || !options_.idle_timeout_ms ||
        !options_.worker_count ||
        !options_.max_stream_queue_frames || !options_.max_stream_queue_bytes)
    {
        LOG_ERROR("[RECORD] init failed, directory=", options_.directory, " error=", error.message());
        state_ = ServiceState::Failed;
        return false;
    }
    state_ = ServiceState::Initialized;
    return true;
}

bool RecordingService::Start()
{
    std::lock_guard<std::mutex> lifecycle(lifecycle_mutex_);
    if (State() == ServiceState::Running) return true;
    if ((State() != ServiceState::Initialized && State() != ServiceState::Stopped) || weak_from_this().expired()) return false;
    std::shared_ptr<RecordingDispatcher> dispatcher;
    try {
        auto context = std::make_shared<RecordingContext>(options_,
            std::chrono::system_clock::now().time_since_epoch().count(),
            written_, dropped_, completed_, errors_);
        dispatcher = std::make_shared<RecordingDispatcher>(
            options_, context, accepted_, dropped_, errors_);
        dispatcher->Start();
        {
            std::lock_guard<std::mutex> lock(mutex_);
            dispatcher_ = dispatcher;
        }
        subscription_ = router_->Subscribe(shared_from_this());
        if (!subscription_) throw std::runtime_error("recording subscription failed");
    } catch (...) {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            dispatcher_.reset();
        }
        if (dispatcher) dispatcher->Stop();
        state_ = ServiceState::Failed;
        return false;
    }
    state_ = ServiceState::Running;
    LOG_INFO("[RECORD] started, directory=", std::filesystem::absolute(options_.directory).string(),
             " workers=", options_.worker_count,
             " segment_ms=", options_.segment_ms, " idle_timeout_ms=", options_.idle_timeout_ms);
    return true;
}

void RecordingService::Stop()
{
    std::lock_guard<std::mutex> lifecycle(lifecycle_mutex_);
    std::shared_ptr<RecordingDispatcher> dispatcher;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        dispatcher = dispatcher_;
    }
    if (!dispatcher) return;
    state_ = ServiceState::Stopping;
    LOG_INFO("[RECORD] stopping, action=unsubscribe-and-drain");
    router_->Unsubscribe(subscription_);
    subscription_ = 0;
    // Stop closes admission atomically with Post, drains accepted jobs and
    // lets each shard finalize the recorders it owns.
    dispatcher->Stop();
    {
        std::lock_guard<std::mutex> lock(mutex_);
        dispatcher_.reset();
    }
    state_ = ServiceState::Stopped;
    LOG_INFO("[RECORD] stopped, written=", written_.load(), " dropped=", dropped_.load(),
             " completed_files=", completed_.load(), " errors=", errors_.load());
}

ServiceHealth RecordingService::Health() const
{
    return {State() == ServiceState::Running && errors_ == 0, Type(), State(), errors_ ? 1 : 0};
}
RecordingStats RecordingService::Stats() const
{
    std::shared_ptr<RecordingDispatcher> dispatcher;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        dispatcher = dispatcher_;
    }
    const auto queued = dispatcher ? dispatcher->Stats() : RecordingDispatcher::QueueStats{};
    return {accepted_, written_, dropped_, completed_, errors_, queued.frames, queued.bytes};
}

bool RecordingService::TryEnqueue(const media::EncodedFrameEvent& event)
{
    if (!event.Valid() || !event.frame->IsComplete() ||
        (event.frame->info.codec != media::CodecType::H264 && event.frame->info.codec != media::CodecType::AAC)) {
        ++dropped_;
        return false;
    }
    std::shared_ptr<RecordingDispatcher> dispatcher;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        dispatcher = dispatcher_;
    }
    return dispatcher && dispatcher->Post(event);
}
}
