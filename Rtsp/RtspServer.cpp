#include "RtspServer.h"

RtspServer::RtspServer(EventLoop* event_loop)
    : TcpServer(event_loop)
{
    //handle initialization if needed rtspconnection
    
}

TcpConnection::Ptr RtspServer:: OnConnect(SOCKET sockfd)
{
    return std::make_shared<RtspConnection>(shared_from_this(), this->GetEventLoop(), sockfd);
}