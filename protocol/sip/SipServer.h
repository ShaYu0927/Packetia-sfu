#ifndef PACKETIA_PROTOCOL_SIP_SIPSERVER_H_
#define PACKETIA_PROTOCOL_SIP_SIPSERVER_H_

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

#endif // PACKETIA_PROTOCOL_SIP_SIPSERVER_H_
