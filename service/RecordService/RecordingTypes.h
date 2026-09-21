#ifndef PACKETIA_SERVICE_RECORDSERVICE_RECORDINGTYPES_H_
#define PACKETIA_SERVICE_RECORDSERVICE_RECORDINGTYPES_H_

#include "RecordingOptions.h"
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <utility>

namespace service 
{

struct RecordingSessionKey
{
    std::string session_id;
    std::string stream_id;

    bool operator==(const RecordingSessionKey& other) const noexcept
    {
        return session_id == other.session_id && stream_id == other.stream_id;
    }
};

struct RecordingInstanceId
{
    RecordingSessionKey session;
    int64_t run_id = 0;
    uint64_t generation = 0;
};

enum class RecordingSessionState
{
    Discovering,
    WaitingForKeyFrame,
    Recording,
    Stopping,
    Stopped,
    Failed
};

enum class RecordingInstanceState
{
    Discovering,
    Recording,
    Finished,
    Failed
};

enum class RecordingSegmentState
{
    Created,
    Writing,
    Finalizing,
    Completed,
    Failed
};

enum class RecordingStopReason
{
    None,
    StreamIdle,
    QueueOverflow,
    SourceGone,
    ServiceStopping,
    InternalError
};

enum class RecordingEventType
{
    SessionStarted,
    SessionRecording,
    SessionStopped,
    SessionFailed,
    SegmentStarted,
    SegmentCompleted,
    SegmentFailed
};

struct RecordingEvent
{
    RecordingEventType type = RecordingEventType::SessionStarted;
    RecordingInstanceId instance;
    RecordingSessionState state = RecordingSessionState::Discovering;
    RecordingStopReason reason = RecordingStopReason::None;
    uint64_t timestamp_ms = 0;
    std::string path;
    std::string error;
};

class IRecordingEventSink
{
public:
    virtual ~IRecordingEventSink() = default;
    virtual void OnRecordingEvent(const RecordingEvent& event) = 0;
};

// Shared across recording workers; immutable configuration and atomics are the
// only cross-shard state. The event sink is invoked on the owning worker.
struct RecordingContext
{
    const RecordingOptions& options;
    const int64_t run_id;
    std::atomic<uint64_t> sequence{0};
    std::atomic<uint64_t> instance_sequence{0};
    std::atomic<size_t> pending_bytes{0};
    std::atomic<uint64_t>& written;
    std::atomic<uint64_t>& dropped;
    std::atomic<uint64_t>& completed;
    std::atomic<uint64_t>& errors;
    std::shared_ptr<IRecordingEventSink> event_sink;

    RecordingContext(const RecordingOptions& opts, int64_t id,
        std::atomic<uint64_t>& w, std::atomic<uint64_t>& d,
        std::atomic<uint64_t>& c, std::atomic<uint64_t>& e,
        std::shared_ptr<IRecordingEventSink> sink = nullptr)
        : options(opts), run_id(id), written(w), dropped(d), completed(c),
          errors(e), event_sink(std::move(sink)) {}

    bool ReservePending(size_t bytes)
    {
        auto used = pending_bytes.load();
        do {
            if (bytes > options.max_pending_bytes - used) return false;
        } while (!pending_bytes.compare_exchange_weak(used, used + bytes));
        return true;
    }
};

}

#endif // PACKETIA_SERVICE_RECORDSERVICE_RECORDINGTYPES_H_
