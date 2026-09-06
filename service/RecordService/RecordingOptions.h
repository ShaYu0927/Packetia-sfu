#pragma once

#include <cstddef>
#include <cstdint>
#include <string>

namespace service {
struct RecordingOptions {
    std::string directory = "recordings";
    // Global admission limits include batches currently executing on workers.
    size_t max_queue_frames = 1024;
    size_t max_queue_bytes = 64 * 1024 * 1024;
    size_t max_streams = 16;
    size_t max_pending_bytes = 64 * 1024 * 1024;
    uint64_t discovery_ms = 1000;
    uint64_t idle_timeout_ms = 3000;
    uint64_t segment_ms = 60000;
    // Dedicated shards provided by the project's ShardedWorkerPool.
    size_t worker_count = 2;
    size_t max_stream_queue_frames = 512;
    size_t max_stream_queue_bytes = 16 * 1024 * 1024;
};
}
