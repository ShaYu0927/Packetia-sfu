#include "RtspServer.h"

RtspServer::RtspServer(EventLoop* event_loop)
    : TcpServer(event_loop)
{
    //handle initialization if needed rtspconnection
    
}

RtspServer::~RtspServer()
{
}

TcpConnection::Ptr RtspServer:: OnConnect(SOCKET sockfd)
{
    return std::make_shared<RtspConnection>(shared_from_this(), this->GetEventLoop()->GetTaskScheduler().get(), sockfd);
}