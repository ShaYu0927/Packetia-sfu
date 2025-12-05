#pragma once

#include "network/TcpConnection.h"

class SipConnection : public TcpConnection{

public:
    SipConnection(SipServer* server, TaskScheduler* scheduler, SOCKET fd);
    ~SipConnection();

};