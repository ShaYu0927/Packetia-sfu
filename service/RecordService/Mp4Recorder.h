#pragma once

#include "RecordingOptions.h"
#include "Mp4Writer.h"
#include <atomic>
#include <map>

namespace service {

// Shared across recording workers; must outlive every recorder. Only the
// immutable options and atomic budgets/counters are shared between workers.
struct RecordingContext 
{
    const RecordingOptions& options;
    const int64_t run_id;
    std::atomic<uint64_t> sequence{0};
    std::atomic<size_t> pending_bytes{0};
    std::atomic<uint64_t>& written;
    std::atomic<uint64_t>& dropped;
    std::atomic<uint64_t>& completed;
    std::atomic<uint64_t>& errors;

    RecordingContext(const RecordingOptions& opts, int64_t id,
        std::atomic<uint64_t>& w, std::atomic<uint64_t>& d,
        std::atomic<uint64_t>& c, std::atomic<uint64_t>& e)
        : options(opts), run_id(id), written(w), dropped(d), completed(c), errors(e) {}

    bool ReservePending(size_t bytes) {
        auto used = pending_bytes.load();
        do {
            if (bytes > options.max_pending_bytes - used) return false;
        } while (!pending_bytes.compare_exchange_weak(used, used + bytes));
        return true;
    }
};

// One stream's recording lifecycle; all methods run on its assigned worker.
class Mp4Recorder 
{
public:
    explicit Mp4Recorder(RecordingContext& context) : context_(context) {}
    ~Mp4Recorder();
    Mp4Recorder(const Mp4Recorder&) = delete;
    Mp4Recorder& operator=(const Mp4Recorder&) = delete;
    void InputFrame(const media::EncodedFrameEvent& event, uint64_t now);
    // Returns true when the recorder has finalized and can be removed.
    bool Tick(uint64_t now, bool stopping);
    void Close();
private:
    void DiscardPending();
    void Fail(const std::string& message);
    void Write(const media::EncodedFrameEvent& event);
    void Open();
    struct Clock
    {
        media::EncodedFrameEvent first;
        uint32_t previous = 0;
        int64_t ticks = 0;
        int64_t anchor_us = 0;
    };


    struct Recording
    {
        std::map<uint64_t, Clock> tracks;
        std::vector<media::EncodedFrameEvent> pending;
        std::unique_ptr<Mp4Writer> writer;
        std::string path;
        uint64_t first_ms = 0, last_ms = 0, frames = 0;
        int64_t origin_us = 0;
        size_t pending_bytes = 0;
        bool video_seen = false, failed = false;
        std::string session_id, stream_id;
    };
    Recording recording_;
    RecordingContext& context_;
};
}
