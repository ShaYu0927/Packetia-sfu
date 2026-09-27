#pragma once

#include "media/core/EncodedFrameRouter.h"

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace service::mix {

// Room membership is supplied explicitly by the application. A publisher's
// session_id is not necessarily a room ID; track IDs are local to an endpoint.
struct MixInput {
    std::string participant_id;
    uint64_t endpoint_id = 0;
    media::TrackId track_id = 0;
    // Required when using the application's shared stream configuration.
    std::string session_id;
    std::string stream_id;
};

struct MixConfig {
    std::string room_id;
    std::string output_stream_id;
    uint64_t output_endpoint_id = 0; // Allocate a distinct nonzero endpoint ID.
    std::vector<MixInput> inputs;
    size_t max_queue_frames = 128;
    size_t max_queue_bytes = 32 * 1024 * 1024;
    uint32_t tick_interval_ms = 10;
};

enum class MixState { Created, Starting, Running, Stopping, Stopped, Failed };

struct MixStats {
    uint64_t accepted = 0;
    uint64_t processed = 0;
    uint64_t rejected = 0;
    uint64_t discarded = 0;
    uint64_t published = 0; // Published encoded output, not completed recordings.
    uint64_t output_rejected = 0;
    size_t queue_frames = 0;
    size_t queue_bytes = 0; // Queued storage only; excludes the executing frame.
};

} // namespace service::mix
