#pragma once

#include "TcpServer.h"

class SipServer : public TcpServer
{
public:
    explicit SipServer(EventLoop* event_loop);
    ~SipServer() override;

protected:
    TcpConnection::Ptr OnConnect(SOCKET sockfd) override;
};