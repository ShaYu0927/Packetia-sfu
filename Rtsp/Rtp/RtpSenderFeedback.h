#pragma once
#include <cstdint>

namespace rtsp
{
// Per-sender RTCP observations. Used by the sender's serialized owner.
struct RtpSenderFeedback
{
    uint64_t rr_count = 0;
    uint32_t reporter_ssrc = 0, media_ssrc = 0;
    uint8_t fraction_lost = 0;
    int32_t cumulative_lost = 0;
    uint32_t highest_seq = 0, jitter = 0, lsr = 0, dlsr = 0, rtt_ms = 0;
    uint64_t last_keyframe_request_ms = 0;
    uint64_t keyframe_interval_ms = 1000;

    bool AllowKeyFrameRequest(uint64_t now_ms)
    {
        if (last_keyframe_request_ms != 0 && now_ms >= last_keyframe_request_ms &&
            now_ms - last_keyframe_request_ms < keyframe_interval_ms) return false;
        last_keyframe_request_ms = now_ms;
        return true;
    }
};
}
