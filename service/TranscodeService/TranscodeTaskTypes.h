#pragma once

#include "TranscodeSource.h"
#include "TranscodeTypes.h"

#include <cstddef>
#include <string>

namespace service::transcode {

struct TranscodeQueueLimits
{
    size_t max_frames = 32;
    size_t max_bytes = 16 * 1024 * 1024;
    bool Valid() const noexcept { return max_frames && max_bytes; }
};

struct TranscodeTaskConfig
{
    std::string task_id;
    // Track IDs are endpoint-local. Optional session/stream/SSRC narrow the match.
    TranscodeSource input;
    TranscodeOutput output;
    VideoTranscodeConfig video = [] {
        VideoTranscodeConfig value;
        value.timestamp_mode = InputTimestampMode::Rtp32;
        return value;
    }();
    TranscodeQueueLimits queue;
};

enum class TranscodeTaskState { Created, Starting, Running, Stopping, Stopped, Failed };

struct TranscodeTaskStats
{
    uint64_t accepted = 0;
    uint64_t processed = 0;
    uint64_t dropped = 0;
    uint64_t decoded_frames = 0;
    uint64_t encoded_frames = 0;
    uint64_t published = 0;
    uint64_t output_unaccepted = 0;
    uint64_t recoveries = 0;
    uint64_t errors = 0;
    size_t queue_frames = 0;
    size_t queue_bytes = 0; // Does not include the currently executing frame.
};

struct TranscodeTaskStatus
{
    std::string task_id;
    TranscodeTaskState state = TranscodeTaskState::Created;
    // Historical field name: means a random-access key frame for ALL codecs.
    bool waiting_for_idr = true;
    TranscodeTaskStats stats;
    std::string last_error;
};

} // namespace service::transcode
