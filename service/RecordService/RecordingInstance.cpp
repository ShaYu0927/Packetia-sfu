#include "RecordingInstance.h"
#include "Mp4Recorder.h"
#include <memory>

namespace service {

RecordingInstance::RecordingInstance(RecordingContext& context,
                                     RecordingInstanceId id)
    : id_(std::move(id)),
      recorder_(std::make_unique<Mp4Recorder>(context, id_)) {}

void RecordingInstance::InputFrame(const media::EncodedFrameEvent& event,
                                   uint64_t now_ms)
{
    if (state_ == RecordingInstanceState::Finished || state_ == RecordingInstanceState::Failed) return;
    recorder_->InputFrame(event, now_ms);
    RefreshState();
}

bool RecordingInstance::Tick(uint64_t now_ms, bool stopping)
{
    if (state_ == RecordingInstanceState::Finished) return true;
    const bool finished = recorder_->Tick(now_ms, stopping);
    RefreshState();
    if (finished && state_ != RecordingInstanceState::Failed)
        state_ = RecordingInstanceState::Finished;
    return finished;
}

void RecordingInstance::Close()
{
    if (state_ == RecordingInstanceState::Finished) return;
    recorder_->Close();
    RefreshState();
    if (state_ != RecordingInstanceState::Failed)
        state_ = RecordingInstanceState::Finished;
}

void RecordingInstance::RefreshState()
{
    if (recorder_->HasFailed()) state_ = RecordingInstanceState::Failed;
    else if (recorder_->IsOpen()) state_ = RecordingInstanceState::Recording;
}

}
