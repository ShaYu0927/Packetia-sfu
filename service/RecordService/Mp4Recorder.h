#pragma once

#include "IRecorder.h"
#include "RecordingTypes.h"
#include "RecordingSegment.h"

namespace service 
{

// Existing MP4 recording instance. RecordingSession supervises its lifecycle;
// a later extraction can move the nested file state into RecordingSegment.
class Mp4Recorder final : public IRecorder
{
public:
    explicit Mp4Recorder(RecordingContext& context, RecordingInstanceId instance)
        : context_(context), instance_(std::move(instance)) {}

    ~Mp4Recorder() override;
    Mp4Recorder(const Mp4Recorder&) = delete;
    Mp4Recorder& operator=(const Mp4Recorder&) = delete;
    void InputFrame(const media::EncodedFrameEvent& event, uint64_t now) override;
    
    bool Tick(uint64_t now, bool stopping) override;
    void Close() override;
    bool IsOpen() const noexcept override { return segment_.IsOpen(); }
    bool HasFailed() const noexcept override { return segment_.HasFailed(); }
private:
    void DiscardPending();
    void Fail(const std::string& message);
    void Write(const media::EncodedFrameEvent& event);
    
    void EmitEvent(RecordingEventType type, RecordingSessionState state, const std::string& error = {});
    

    bool CanOpen(uint64_t now, bool force) const;
    bool HasReadyVideoTrack() const;
    void Open();
    RecordingSegment segment_;
    RecordingContext& context_;
    RecordingInstanceId instance_;
};

}
