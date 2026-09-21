#ifndef PACKETIA_SERVICE_RECORDSERVICE_RECORDINGSESSION_H_
#define PACKETIA_SERVICE_RECORDSERVICE_RECORDINGSESSION_H_

#include "RecordingInstance.h"
#include "RecordingTypes.h"
#include <memory>

namespace service {

// Supervisor for one logical communication stream. All methods execute on the
// dispatcher shard selected by RecordingSessionKey, so this class needs no lock.
class RecordingSession final
{
public:
    RecordingSession(RecordingContext& context,
                     RecordingSessionKey key,
                     uint64_t generation,
                     uint64_t now_ms);
    ~RecordingSession();

    RecordingSession(const RecordingSession&) = delete;
    RecordingSession& operator=(const RecordingSession&) = delete;

    void InputFrame(const media::EncodedFrameEvent& event, uint64_t now_ms);
    // Returns true when the session reached a terminal state and can retire.
    bool Tick(uint64_t now_ms, bool stopping);
    void Close(RecordingStopReason reason, uint64_t now_ms);

    const RecordingInstanceId& Id() const noexcept { return id_; }
    RecordingSessionState State() const noexcept { return state_; }
    RecordingStopReason StopReason() const noexcept { return stop_reason_; }

private:
    void RefreshState(uint64_t now_ms);
    void Transition(RecordingSessionState state,
                    RecordingEventType event,
                    uint64_t now_ms,
                    RecordingStopReason reason = RecordingStopReason::None);
    bool Matches(const media::EncodedFrameEvent& event) const noexcept;

    RecordingContext& context_;
    RecordingInstanceId id_;
    RecordingSessionState state_ = RecordingSessionState::Discovering;
    RecordingStopReason stop_reason_ = RecordingStopReason::None;
    std::unique_ptr<RecordingInstance> instance_;
};

}

#endif // PACKETIA_SERVICE_RECORDSERVICE_RECORDINGSESSION_H_
