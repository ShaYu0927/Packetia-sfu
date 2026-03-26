#ifndef _MEDIAENDPOINT_H_
#define _MEDIAENDPOINT_H_

#include "EndpointBase.h"

namespace media 
{

class MediaEndpoint : public utils::EndpointBase
{
public:
    using EndpointBase::EndpointBase;

    void OnRtp(WorkJob& job) override;
    void OnRtcp(WorkJob& job) override;
    void OnStun(WorkJob& job) override;
    void OnDtls(WorkJob& job) override;

protected:
    virtual void HandleRtpPacket(Packet* pkt) = 0;
    virtual void HandleRtcpPacket(Packet* pkt) = 0;
    virtual void HandleStunPacket(Packet* pkt) {}
    virtual void HandleDtlsPacket(Packet* pkt) {}
};

class SfuEndpoint : public MediaEndpoint
{
public:
    using MediaEndpoint::MediaEndpoint;

    bool Start() override;
    void Stop() override;

protected:
    void HandleRtpPacket(Packet* pkt) override;
    void HandleRtcpPacket(Packet* pkt) override;

private:
    
};

}

#endif /* _MEDIAENDPOINT_H_ */