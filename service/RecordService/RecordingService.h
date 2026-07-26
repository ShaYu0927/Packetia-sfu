#ifndef _RECORDING_SERVICE_H_
#define _RECORDING_SERVICE_H_


#include "H264Payload.h"
#include "core/IService.h"
#include "Mp4.h"
#include <memory>
#include <mutex>

namespace service 
{
class RecordingService : public IService
{
public:
    RecordingService();
    ~RecordingService() override;

    bool Init() override;
    bool Start() override;

    void Stop() override;
    void Shutdown() override;

    service::ServiceType    Type() const override;
    service::ServiceState   State() const override;
    service::ServiceHealth  Health() const override;

    bool StartRecording(const RecordingRequest& request);
    bool StopRecording(const  StreamKey& stream_key);
    void OnVideoFrame(const   StreamKey& stream_key, const media::H264AccessUnit& frame);

private:
    service::ServiceState state_ = service::ServiceState::Created;
    mutable std::mutex mutex_;
};

}


#endif /* _RECORDING_SERVICE_H_ */