// Test-only persistence substitute. The real RecordingService, dispatcher,
// worker pool and session lifecycle run unchanged; no FFmpeg/muxing is tested.
#include "service/RecordService/RecordingInstance.h"

namespace service {
RecordingInstance::RecordingInstance(RecordingContext&, RecordingInstanceId id) : id_(std::move(id)) {}
void RecordingInstance::InputFrame(const media::EncodedFrameEvent&, uint64_t) { state_ = RecordingInstanceState::Recording; }
bool RecordingInstance::Tick(uint64_t, bool stopping) { return stopping; }
void RecordingInstance::Close() { state_ = RecordingInstanceState::Finished; }
void RecordingInstance::RefreshState() {}
}
