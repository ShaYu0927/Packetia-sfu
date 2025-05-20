#include "RtspServer.h"

RtspServer::RtspServer(EventLoop* event_loop)
    : TcpServer(event_loop)
{

}

TcpConnection::Ptr RtspServer:: OnConnect(SOCKET sockfd)
{

}