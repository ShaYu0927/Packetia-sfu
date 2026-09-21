#ifndef PACKETIA_SERVICE_RECORDSERVICE_IRECORDER_H_
#define PACKETIA_SERVICE_RECORDSERVICE_IRECORDER_H_

#include <cstdint>

namespace media {
struct EncodedFrameEvent;
}

namespace service {

// Stable boundary between recording orchestration and a concrete recorder.
class IRecorder
{
public:
    virtual ~IRecorder() = default;

    virtual void InputFrame(const media::EncodedFrameEvent& event,
                            uint64_t now_ms) = 0;
    virtual bool Tick(uint64_t now_ms, bool stopping) = 0;
    virtual void Close() = 0;
    virtual bool IsOpen() const noexcept = 0;
    virtual bool HasFailed() const noexcept = 0;
};

}

#endif // PACKETIA_SERVICE_RECORDSERVICE_IRECORDER_H_
