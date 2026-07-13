#ifndef _I_SERVICE_H_
#define _I_SERVICE_H_


#include <cstdint>

namespace service {



enum class ServiceType : uint16_t
{
    Unknown = 0,

    Network = 100,
    Worker  = 200,

    Media   = 300,
    Rtsp    = 301,
    WebRtc  = 302,
    Sip     = 303,

    Ai      = 400,
    Asr     = 401,
    Vision  = 402,

    Record  = 500,
    Monitor = 600
};

constexpr uint16_t ToServiceId(ServiceType type)
{
    return static_cast<uint16_t>(type);
}

enum class ServiceState : uint8_t
{
    Created = 0,
    Initialized,
    Starting,
    Running,
    Stopping,
    Stopped,
    Failed
};

struct ServiceHealth
{
    bool healthy = false;

    ServiceType type = ServiceType::Unknown;
    ServiceState state = ServiceState::Created;

    int error_code = 0;
};

class IService
{
public:
    virtual ~IService() = default;

    virtual bool Init() = 0;
    virtual bool Start() = 0;

    virtual void Stop() = 0;
    virtual void Shutdown() = 0;

    virtual ServiceType Type() const = 0;
    virtual ServiceState State() const = 0;
    virtual ServiceHealth Health() const = 0;
};

}

#endif /* _I_SERVICE_H_ */