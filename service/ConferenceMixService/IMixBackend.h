#pragma once

#include "MixTypes.h"

#include <functional>

namespace service::mix {

// Extension point for real decoding, timeline alignment, composition, mixing
// and re-encoding. No production backend is implemented by the framework.
class IMixBackend {
public:
    using Output = std::function<bool(const media::EncodedFrame::ConstPtr&)>;
    virtual ~IMixBackend() = default;

    // All methods run serially on the mixer worker. Output may be called only
    // synchronously from InputFrame, Tick or Stop, never from a backend thread.
    // Start returning false, or Start/InputFrame/Tick throwing, fails the task.
    // Output returns whether at least one downstream sink accepted the frame.
    virtual bool Start(const MixConfig& config, Output output) = 0;
    virtual void InputFrame(const media::EncodedFrameEvent& event) = 0;
    virtual void Tick(uint64_t elapsed_us) = 0; // Monotonic task time, not RTP time.

    // Always invoked after a Start attempt, including partial initialization.
    // Flush encoders on normal stop and release/detach all output callbacks.
    virtual void Stop() noexcept = 0;
};

} // namespace service::mix
