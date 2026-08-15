#pragma once

#include "TcpServer.h"
#include "SipSession.h"

#include <unordered_map>

class SipServer : public TcpServer
{
public:
    explicit SipServer(EventLoop* event_loop);
    ~SipServer() override;

protected:
    TcpConnection::Ptr OnConnect(SOCKET sockfd) override;
    void RemoveConnection(SOCKET sockfd) override;

private:
    std::unordered_map<SOCKET, std::shared_ptr<sip::SipSession>> sessions_;
};
