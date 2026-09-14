#pragma once

#include "IRecorder.h"
#include "RecordingTypes.h"
#include <memory>

namespace service {

// One concrete publisher generation. It adapts encoded media into the current
// MP4 implementation and reports whether the generation can keep running.
class RecordingInstance final
{
public:
    RecordingInstance(RecordingContext& context, RecordingInstanceId id);

    RecordingInstance(const RecordingInstance&) = delete;
    RecordingInstance& operator=(const RecordingInstance&) = delete;

    void InputFrame(const media::EncodedFrameEvent& event, uint64_t now_ms);
    bool Tick(uint64_t now_ms, bool stopping);
    void Close();

    const RecordingInstanceId& Id() const noexcept { return id_; }
    RecordingInstanceState State() const noexcept { return state_; }
    bool IsRecording() const noexcept { return state_ == RecordingInstanceState::Recording; }
    bool HasFailed() const noexcept { return state_ == RecordingInstanceState::Failed; }

private:
    void RefreshState();

    RecordingInstanceId id_;
    RecordingInstanceState state_ = RecordingInstanceState::Discovering;
    std::unique_ptr<IRecorder> recorder_;
};

}
