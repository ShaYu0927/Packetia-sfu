#ifndef _RTSPSERVER_H_
#define _RTSPSERVER_H_


#include <string>
#include <memory>
#include "TcpServer.h"
#include "TcpConnection.h"
#include "RtspConnection.h"

class RtspServer : public TcpServer, public std::enable_shared_from_this<RtspServer>
{
public:
    RtspServer(EventLoop* event_loop);
    virtual ~RtspServer();

  
protected:
    TcpConnection::Ptr OnConnect(SOCKET sockfd) override;


};


#endif