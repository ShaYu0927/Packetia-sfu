#ifndef PACKETIA_PROTOCOL_SIP_SIPCONNECTION_H_
#define PACKETIA_PROTOCOL_SIP_SIPCONNECTION_H_

#include "TcpConnection.h"
#include "SipMessage.h"
#include "TcpServer.h"
#include <memory.h>

class SipConnection : public TcpConnection
{

public:
    SipConnection(TcpServer* server, TaskScheduler* scheduler, SOCKET fd);
    ~SipConnection();


private:
    std::unique_ptr<SipRequest> sip_request_;
    std::unique_ptr<SipResponse> sip_response_;
};

#endif // PACKETIA_PROTOCOL_SIP_SIPCONNECTION_H_
