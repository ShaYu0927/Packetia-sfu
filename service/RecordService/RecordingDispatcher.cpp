#include "RecordingDispatcher.h"
#include "logger.h"
#include <chrono>
#include <stdexcept>
#include <utility>

namespace service {
namespace {
uint64_t NowMs()
{
    return std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count();
}
}

struct RecordingDispatcher::StreamEntry
{
    explicit StreamEntry(Key value) : key(std::move(value)) {}
    Key key;
    size_t frames = 0;
    size_t bytes = 0;
    uint64_t epoch = 0;
    bool restart = false;
    bool active = false;
};

struct RecordingDispatcher::FrameJob
{
    media::EncodedFrameEvent event;
    std::shared_ptr<StreamEntry> stream;
    uint64_t epoch = 0;
};

class RecordingDispatcher::JobHandler final : public IJobHandler
{
public:
    struct Session
    {
        std::shared_ptr<StreamEntry> stream;
        std::unique_ptr<Mp4Recorder> recorder;
    };
    using Sessions = std::map<Key, Session>;

    JobHandler(RecordingDispatcher& owner, size_t workers)
        : owner_(owner), sessions_(workers) {}

    void handle(WorkJob&) override {}

    void handle(WorkJob& job, size_t worker) override
    {
        if (job.type != WorkType::Recording || worker >= sessions_.size()) return;
        auto frame = std::static_pointer_cast<FrameJob>(job.owner);
        if (!frame || !frame->event.Valid()) return;

        bool stale = false;
        bool restart = false;
        {
            std::lock_guard<std::mutex> lock(owner_.mutex_);
            stale = frame->epoch != frame->stream->epoch;
            if (!stale && frame->stream->restart) 
            {
                restart = true;
                frame->stream->restart = false;
            }
        }

        auto& shard = sessions_[worker];
        auto found = shard.find(frame->stream->key);
        if (stale) 
        {
            ++owner_.dropped_;
            owner_.Complete(frame->stream, frame->event.frame->size);
            return;
        }

        try {
            if (restart && found != shard.end()) 
            {
                found->second.recorder->Close();
                shard.erase(found);
                found = shard.end();
            }
            if (found == shard.end()) 
            {
                Session session;
                session.stream = frame->stream;
                session.recorder = std::make_unique<Mp4Recorder>(*owner_.context_);
                found = shard.emplace(frame->stream->key, std::move(session)).first;
                std::lock_guard<std::mutex> lock(owner_.mutex_);
                frame->stream->active = true;
            }
            const auto now = NowMs();
            found->second.recorder->InputFrame(frame->event, now);
            if (found->second.recorder->Tick(now, false)) 
            {
                shard.erase(found);
                MarkInactive(frame->stream);
            }
        } catch (const std::exception& error) 
        {
            LOG_ERROR("[RECORD] worker task failed, session=", frame->stream->key.first,
                      " stream=", frame->stream->key.second, " error=", error.what());
            ++owner_.errors_;
            if (found != shard.end()) shard.erase(found);
            MarkInactive(frame->stream);
        } catch (...) 
        {
            LOG_ERROR("[RECORD] worker task failed, session=", frame->stream->key.first,
                      " stream=", frame->stream->key.second, " error=unknown");
            ++owner_.errors_;
            if (found != shard.end()) shard.erase(found);
            MarkInactive(frame->stream);
        }
        owner_.Complete(frame->stream, frame->event.frame->size);
    }

    void on_worker_tick(size_t worker) override
    {
        if (worker >= sessions_.size()) return;
        auto& shard = sessions_[worker];
        const auto now = NowMs();
        for (auto it = shard.begin(); it != shard.end();) {
            try {
                if (!it->second.recorder->Tick(now, false)) { ++it; continue; }
            } catch (const std::exception& error) {
                LOG_ERROR("[RECORD] tick failed, session=", it->first.first,
                          " stream=", it->first.second, " error=", error.what());
                ++owner_.errors_;
            } catch (...) {
                ++owner_.errors_;
            }
            auto stream = it->second.stream;
            it = shard.erase(it);
            MarkInactive(stream);
        }
    }

    void on_worker_stop(size_t worker) override
    {
        if (worker >= sessions_.size()) return;
        for (auto& item : sessions_[worker]) {
            try {
                if (!item.second.recorder->Tick(NowMs(), true))
                    item.second.recorder->Close();
            } catch (...) {
                ++owner_.errors_;
            }
            MarkInactive(item.second.stream);
        }
        sessions_[worker].clear();
    }

private:
    void MarkInactive(const std::shared_ptr<StreamEntry>& stream)
    {
        {
            std::lock_guard<std::mutex> lock(owner_.mutex_);
            stream->active = false;
        }
        owner_.Retire(stream->key, stream);
    }

    RecordingDispatcher& owner_;
    std::vector<Sessions> sessions_;
};

RecordingDispatcher::RecordingDispatcher(const RecordingOptions& options,
    std::shared_ptr<RecordingContext> context,
    std::atomic<uint64_t>& accepted, std::atomic<uint64_t>& dropped,
    std::atomic<uint64_t>& errors)
    : options_(options), context_(std::move(context)), accepted_(accepted),
      dropped_(dropped), errors_(errors) {}

RecordingDispatcher::~RecordingDispatcher() { Stop(); }

void RecordingDispatcher::Start()
{
    std::lock_guard<std::mutex> lock(mutex_);
    if (started_) throw std::logic_error("recording dispatcher already started");
    if (!context_ || !options_.worker_count || !options_.max_queue_frames ||
        !options_.max_queue_bytes || !options_.max_stream_queue_frames ||
        !options_.max_stream_queue_bytes || !options_.max_streams)
        throw std::invalid_argument("invalid recording dispatcher limits");
    handler_ = std::make_shared<JobHandler>(*this, options_.worker_count);
    if (WorkerService::exists("recording") ||
        WorkerService::create_pool("recording", options_.worker_count, handler_,
            options_.max_queue_frames, ShardedWorkerPool::DropPolicy::DropTail) != 0) {
        handler_.reset();
        throw std::runtime_error("start recording worker module failed");
    }
    started_ = true;
    accepting_ = true;
}

void RecordingDispatcher::Stop()
{
    bool started = false;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        accepting_ = false;
        started = std::exchange(started_, false);
    }
    if (started) WorkerService::destroy_pool("recording", true);
    std::lock_guard<std::mutex> lock(mutex_);
    handler_.reset();
    streams_.clear();
    queued_ = {};
}

bool RecordingDispatcher::Post(const media::EncodedFrameEvent& event)
{
    if (!event.Valid()) return false;
    std::shared_ptr<StreamEntry> stream;
    FrameJob payload;
    WorkJob job{};
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!accepting_ || !started_) return false;
        const Key key{event.source.session_id, event.source.stream_id};
        auto found = streams_.find(key);
        const bool new_stream = found == streams_.end();
        if (new_stream && streams_.size() >= options_.max_streams) {
            ++dropped_;
            return false;
        }
        if (new_stream) found = streams_.emplace(key, std::make_shared<StreamEntry>(key)).first;
        stream = found->second;
        const size_t bytes = event.frame->size;
        const bool full = queued_.frames >= options_.max_queue_frames ||
            bytes > options_.max_queue_bytes - queued_.bytes ||
            stream->frames >= options_.max_stream_queue_frames ||
            bytes > options_.max_stream_queue_bytes - stream->bytes;
        if (full) {
            ++stream->epoch;
            stream->restart = true;
            ++dropped_;
            return false;
        }
        ++stream->frames;
        stream->bytes += bytes;
        ++queued_.frames;
        queued_.bytes += bytes;
        payload = FrameJob{event, stream, stream->epoch};
        job.key = AffinityKey(key);
        job.type = WorkType::Recording;
        job.owner = std::make_shared<FrameJob>(std::move(payload));
        ++accepted_;
        if (WorkerService::post("recording", std::move(job)) == 0) return true;
        --accepted_;
        --stream->frames;
        stream->bytes -= bytes;
        --queued_.frames;
        queued_.bytes -= bytes;
        ++dropped_;
        if (!stream->frames && !stream->active) streams_.erase(key);
    }
    return false;
}

RecordingDispatcher::QueueStats RecordingDispatcher::Stats() const
{
    std::lock_guard<std::mutex> lock(mutex_);
    return queued_;
}

void RecordingDispatcher::Complete(const std::shared_ptr<StreamEntry>& stream, size_t bytes)
{
    std::lock_guard<std::mutex> lock(mutex_);
    --stream->frames;
    stream->bytes -= bytes;
    --queued_.frames;
    queued_.bytes -= bytes;
    if (!stream->frames && !stream->active) streams_.erase(stream->key);
}

void RecordingDispatcher::Retire(const Key& key, const std::shared_ptr<StreamEntry>& stream)
{
    std::lock_guard<std::mutex> lock(mutex_);
    auto found = streams_.find(key);
    if (found != streams_.end() && found->second == stream &&
        !stream->frames && !stream->active) streams_.erase(found);
}

uint64_t RecordingDispatcher::AffinityKey(const Key& key)
{
    const auto first = static_cast<uint64_t>(std::hash<std::string>{}(key.first));
    const auto second = static_cast<uint64_t>(std::hash<std::string>{}(key.second));
    return first ^ (second + 0x9e3779b97f4a7c15ULL + (first << 6) + (first >> 2));
}

}
