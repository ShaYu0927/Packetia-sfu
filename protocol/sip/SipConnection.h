#pragma once

#include "network/TcpConnection.h"
#include "SipMessage.h"

#include <memory.h>

class SipConnection : public TcpConnection{

public:
    SipConnection(SipServer* server, TaskScheduler* scheduler, SOCKET fd);
    ~SipConnection();


private:
    std::unique_ptr<SipRequest> sip_request_;
    std::unique_ptr<SipResponse> sip_response_;

};