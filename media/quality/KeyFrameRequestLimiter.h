#ifndef PACKETIA_KEY_FRAME_REQUEST_LIMITER_H_
#define PACKETIA_KEY_FRAME_REQUEST_LIMITER_H_

#include <cstdint>
#include <optional>

namespace media
{
// Serialize with the owning stream. Multiple subscribers must share the
// publisher's limiter so that each subscriber cannot trigger a keyframe burst.
class KeyFrameRequestLimiter
{
public:
    explicit KeyFrameRequestLimiter(uint64_t interval_ms = 1000) : interval_ms_(interval_ms) {}

    bool Allow(uint64_t now_ms)
    {
        if (last_request_ms_ && now_ms >= *last_request_ms_ && now_ms - *last_request_ms_ < interval_ms_)
            return false;
        last_request_ms_ = now_ms;
        return true;
    }

private:
    uint64_t interval_ms_;
    std::optional<uint64_t> last_request_ms_;
};
}

#endif
