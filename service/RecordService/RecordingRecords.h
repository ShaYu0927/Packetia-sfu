#pragma once
#include <cstdint>
#include <string>
#include <vector>

namespace service {
enum class SegmentStatus { Writing, Completed, Failed };
struct RecordingTrackInfo {
    uint64_t endpoint_id = 0;
    uint32_t track_id = 0;
    std::string codec;
    int width = 0, height = 0, sample_rate = 0, channels = 0;
};
struct SegmentInfo {
    std::string recording_id, segment_id, session_id, stream_id;
    uint64_t sequence = 0;
    // Media positions are relative to this stream recording, not another RTP clock.
    int64_t media_start_us = 0, media_end_us = 0;
    int64_t started_at_ms = 0, ended_at_ms = 0; // UTC, anchored at receipt of first media.
    std::string relative_path;
    uint64_t size_bytes = 0;
    SegmentStatus status = SegmentStatus::Writing;
    std::vector<RecordingTrackInfo> tracks;
    std::string error;
};
struct SegmentQuery {
    std::string session_id, stream_id, recording_id;
    int64_t from_ms = 0, to_ms = INT64_MAX; // UTC overlap, [from, to).
    bool completed_only = true;
    uint32_t limit = 100, offset = 0;
};
struct RecordedStream {
    std::string recording_id, session_id, stream_id;
};
} // namespace service
