#include "RecordingService.h"
#include "RecordingTypes.h"
#include "RecordingDispatcher.h"
#include "RecordingCatalog.h"
#include "logger.h"
#include <chrono>
#include <filesystem>
#include <stdexcept>
#include <utility>

namespace service {
RecordingService::RecordingService(std::shared_ptr<media::EncodedFrameRouter> router,
                                   RecordingOptions options,
                                   std::shared_ptr<IRecordingEventSink> event_sink,
                                   std::shared_ptr<config::ConfigStore> config)
    : router_(std::move(router)), options_(std::move(options)),
      event_sink_(std::move(event_sink)), config_(std::move(config)) {}
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
        !options_.max_stream_queue_frames || !options_.max_stream_queue_bytes ||
        options_.reorder_ms > 5000 || options_.segment_ms > static_cast<uint64_t>(INT64_MAX / 1000))
    {
        LOG_ERROR("[RECORD] init failed, directory=", options_.directory, " error=", error.message());
        state_ = ServiceState::Failed;
        return false;
    }
    try {
        const auto path = options_.index_path.empty()
            ? (std::filesystem::path(options_.directory) / ".index" / "recordings.sqlite").string()
            : options_.index_path;
        auto catalog = std::make_shared<RecordingCatalog>(path);
        std::lock_guard<std::mutex> lock(mutex_);
        catalog_ = std::move(catalog);
    } catch (const std::exception& e) {
        LOG_ERROR("[RECORD] index initialization failed: ", e.what());
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
            written_, dropped_, completed_, errors_, event_sink_);
        context->catalog = catalog_;
        dispatcher = std::make_shared<RecordingDispatcher>(options_, context, accepted_, dropped_, errors_, config_);
        dispatcher->Start();
        {
            std::lock_guard<std::mutex> lock(mutex_);
            dispatcher_ = dispatcher;
        }
        subscription_ = router_->Subscribe(shared_from_this());
        if (!subscription_) throw std::runtime_error("recording subscription failed");
    } 
    catch (...) 
    {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            dispatcher_.reset();
        }
        if (dispatcher) dispatcher->Stop();
        state_ = ServiceState::Failed;
        return false;
    }
    state_ = ServiceState::Running;
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

bool RecordingService::SubmitFrame(const media::EncodedFrameEvent& event)
{
    if (event.source.session_id.empty() || event.source.stream_id.empty()) return false;
    if (config_ && !config_->Allows(config::Feature::Recording,
        {event.source.session_id, event.source.stream_id})) return false;
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

void RecordingService::RefreshStreamPolicies()
{
    std::lock_guard<std::mutex> lifecycle(lifecycle_mutex_);
    std::shared_ptr<RecordingDispatcher> dispatcher;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        dispatcher = dispatcher_;
    }
    if (dispatcher) dispatcher->RefreshStreamPolicies();
}

bool RecordingService::StartRecording(const config::StreamKey& stream)
{
    std::lock_guard<std::mutex> lifecycle(lifecycle_mutex_);
    if (!config_ || stream.session_id.empty() || stream.stream_id.empty() ||
        State() != ServiceState::Running || !config_->Snapshot()->recording_enabled) return false;
    config_->SetStreamRecording(stream, true);
    return true;
}

bool RecordingService::StopRecording(const config::StreamKey& stream)
{
    std::lock_guard<std::mutex> lifecycle(lifecycle_mutex_);
    if (!config_ || stream.session_id.empty() || stream.stream_id.empty()) return false;
    std::shared_ptr<RecordingDispatcher> dispatcher;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        dispatcher = dispatcher_;
    }
    auto disable = [&] { config_->SetStreamRecording(stream, false); };
    if (!dispatcher) { disable(); return true; }
    return dispatcher->DrainStream(stream, disable);
}

std::vector<SegmentInfo> RecordingService::QuerySegments(const SegmentQuery& query) const
{
    std::shared_ptr<RecordingCatalog> catalog;
    { std::lock_guard<std::mutex> lock(mutex_); catalog = catalog_; }
    if (!catalog) throw std::logic_error("recording index is not initialized");
    return catalog->Query(query);
}

std::vector<RecordedStream> RecordingService::ListRecordedStreams(const std::string& session_id) const
{
    std::shared_ptr<RecordingCatalog> catalog;
    { std::lock_guard<std::mutex> lock(mutex_); catalog = catalog_; }
    if (!catalog) throw std::logic_error("recording index is not initialized");
    return catalog->Streams(session_id);
}
}
