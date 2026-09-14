#include "RecordingSession.h"
#include "EncodedFrameRouter.h"
#include "logger.h"

namespace service 
{

RecordingSession::RecordingSession(RecordingContext& context,
                                   RecordingSessionKey key,
                                   uint64_t generation,
                                   uint64_t now_ms)
    : context_(context), id_{std::move(key), context.run_id, generation}
{
    instance_ = std::make_unique<RecordingInstance>(context_, id_);
    Transition(RecordingSessionState::Discovering,
               RecordingEventType::SessionStarted, now_ms);
}

RecordingSession::~RecordingSession() = default;

bool RecordingSession::Matches(const media::EncodedFrameEvent& event) const noexcept
{
    return event.source.session_id == id_.session.session_id &&
           event.source.stream_id == id_.session.stream_id;
}

void RecordingSession::InputFrame(const media::EncodedFrameEvent& event, uint64_t now_ms)
{
    if (!instance_ || !Matches(event) ||
        state_ == RecordingSessionState::Stopping ||
        state_ == RecordingSessionState::Stopped ||
        state_ == RecordingSessionState::Failed) {
        ++context_.dropped;
        return;
    }

    const size_t frame_size = event.frame ? event.frame->size : 0;
    const void* frame_id = event.frame ? static_cast<const void*>(event.frame.get()) : nullptr;

    LOG_INFO("[RECORD_FLOW] Session::InputFrame enter"
              ", session=", static_cast<const void*>(this),
              ", instance=", static_cast<const void*>(instance_.get()),
              ", frame=", frame_id,
              ", bytes=", frame_size,
              ", state=", static_cast<int>(state_));

    instance_->InputFrame(event, now_ms);
    RefreshState(now_ms);
}

bool RecordingSession::Tick(uint64_t now_ms, bool stopping)
{
    if (!instance_) return true;
    if (stopping && state_ != RecordingSessionState::Stopping)
        state_ = RecordingSessionState::Stopping;
    const bool finished = instance_->Tick(now_ms, stopping);
    RefreshState(now_ms);
    if (!finished) return false;

    stop_reason_ = stopping ? RecordingStopReason::ServiceStopping
                            : RecordingStopReason::StreamIdle;
    const bool failed = instance_->HasFailed();
    if (!failed || state_ != RecordingSessionState::Failed)
        Transition(failed ? RecordingSessionState::Failed
                          : RecordingSessionState::Stopped,
                   failed ? RecordingEventType::SessionFailed
                          : RecordingEventType::SessionStopped,
                   now_ms, stop_reason_);
    instance_.reset();
    return true;
}

void RecordingSession::Close(RecordingStopReason reason, uint64_t now_ms)
{
    if (!instance_ || state_ == RecordingSessionState::Stopped ||
        state_ == RecordingSessionState::Failed) return;
    state_ = RecordingSessionState::Stopping;
    instance_->Close();
    stop_reason_ = reason;
    Transition(instance_->HasFailed() ? RecordingSessionState::Failed
                                      : RecordingSessionState::Stopped,
               instance_->HasFailed() ? RecordingEventType::SessionFailed
                                      : RecordingEventType::SessionStopped,
               now_ms, reason);
    instance_.reset();
}

void RecordingSession::RefreshState(uint64_t now_ms)
{
    if (!instance_) return;
    if (instance_->HasFailed() && state_ != RecordingSessionState::Failed) {
        Transition(RecordingSessionState::Failed,
                   RecordingEventType::SessionFailed, now_ms,
                   RecordingStopReason::InternalError);
    } else if (instance_->IsRecording() && state_ != RecordingSessionState::Recording) {
        Transition(RecordingSessionState::Recording,
                   RecordingEventType::SessionRecording, now_ms);
    } else if (!instance_->IsRecording() && state_ == RecordingSessionState::Discovering) {
        state_ = RecordingSessionState::WaitingForKeyFrame;
    }
}

void RecordingSession::Transition(RecordingSessionState state,
                                  RecordingEventType event,
                                  uint64_t now_ms,
                                  RecordingStopReason reason)
{
    state_ = state;
    if (!context_.event_sink) return;
    try {
        context_.event_sink->OnRecordingEvent(
            RecordingEvent{event, id_, state, reason, now_ms, {}, {}});
    } catch (...) {
        LOG_ERROR("[RECORD] event sink failed, session=", id_.session.session_id,
                  " stream=", id_.session.stream_id,
                  " generation=", id_.generation);
    }
}

}
