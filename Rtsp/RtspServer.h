#ifndef _RTSPSERVER_H_
#define _RTSPSERVER_H_

#include <memory>
#include "TcpServer.h"
#include "TcpConnection.h"
#include "RtspConnection.h"
#include "RtspSession.h"



class RtspServer : public TcpServer, public std::enable_shared_from_this<RtspServer>
{
public:
    RtspServer(EventLoop* event_loop);
    virtual ~RtspServer();
protected:
    TcpConnection::Ptr OnConnect(SOCKET sockfd) override;
    void RemoveConnection(SOCKET sockfd) override;
    std::unordered_map<int, std::shared_ptr<rtsp::RtspSession>> sessions_;
};


#endif
