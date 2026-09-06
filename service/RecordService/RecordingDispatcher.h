#pragma once

#include "Mp4Recorder.h"
#include "ShardedWorkerPool.h"
#include <atomic>
#include <map>
#include <memory>
#include <mutex>
#include <vector>

namespace service {

// Recording business module running on the project's ShardedWorkerPool.
// Admission is concurrent; each recorder is owned exclusively by one shard.
class RecordingDispatcher final
{
public:
    struct QueueStats { size_t frames = 0, bytes = 0; };

    RecordingDispatcher(const RecordingOptions& options,
        std::shared_ptr<RecordingContext> context,
        std::atomic<uint64_t>& accepted,
        std::atomic<uint64_t>& dropped,
        std::atomic<uint64_t>& errors);
    ~RecordingDispatcher();
    RecordingDispatcher(const RecordingDispatcher&) = delete;
    RecordingDispatcher& operator=(const RecordingDispatcher&) = delete;

    void Start();
    void Stop();
    bool Post(const media::EncodedFrameEvent& event);
    QueueStats Stats() const;

private:
    using Key = std::pair<std::string, std::string>;
    struct StreamEntry;
    struct FrameJob;
    class JobHandler;

    static uint64_t AffinityKey(const Key& key);
    void Complete(const std::shared_ptr<StreamEntry>& entry, size_t bytes);
    void Retire(const Key& key, const std::shared_ptr<StreamEntry>& entry);

    const RecordingOptions options_;
    std::shared_ptr<RecordingContext> context_;
    std::atomic<uint64_t>& accepted_;
    std::atomic<uint64_t>& dropped_;
    std::atomic<uint64_t>& errors_;
    mutable std::mutex mutex_;
    bool accepting_ = false;
    QueueStats queued_;
    std::map<Key, std::shared_ptr<StreamEntry>> streams_;
    std::shared_ptr<JobHandler> handler_;
    bool started_ = false;
};

}
