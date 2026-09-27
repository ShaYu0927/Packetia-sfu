#ifndef PACKETIA_SERVICE_RECORDSERVICE_MP4RECORDER_H_
#define PACKETIA_SERVICE_RECORDSERVICE_MP4RECORDER_H_

#include "IRecorder.h"
#include "RecordingTypes.h"
#include "RecordingSegment.h"
#include <map>
#include <limits>

namespace service 
{

// One stream's segment lifecycle. The dispatcher exclusively owns this object
// on one worker; Mp4Writer handles only muxing, RecordingCatalog only indexing.
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
    struct PendingFrame { media::EncodedFrameEvent event; int64_t time_us; uint64_t received_ms; };
    void DiscardPending();
    void Fail(const std::string& message);
    void Write(const PendingFrame& frame);
    void Drain(uint64_t now, bool force);
    bool OpenSegment(int64_t origin_us);
    bool FinalizeSegment();
    void Store();
    void EmitEvent(RecordingEventType type, RecordingSessionState state, const std::string& error = {});
    void Open();
    std::multimap<std::pair<int64_t, int>, PendingFrame> pending_;
    int64_t latest_us_ = std::numeric_limits<int64_t>::min();
    std::pair<int64_t, int> drained_key_{std::numeric_limits<int64_t>::min(), -1};
    int64_t recording_origin_us_ = 0;
    int64_t wall_anchor_ms_ = 0, media_anchor_us_ = 0;
    bool clock_anchored_ = false, discovered_ = false, closed_ = false;
    uint64_t segment_sequence_ = 0;
    SegmentInfo info_;
    struct WrittenTrack { int64_t last_us = 0, duration_us = 0; };
    std::map<RecordingTrackKey, WrittenTrack> written_tracks_;
    RecordingSegment segment_;
    RecordingContext& context_;
    RecordingInstanceId instance_;
};

}

#endif // PACKETIA_SERVICE_RECORDSERVICE_MP4RECORDER_H_
