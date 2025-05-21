#ifndef _RTSPCONNECTION_H_
#define _RTSPCONNECTION_H_

#include "TcpConnection.h"
#include "BufferWrite.h"
#include "EventLoop.h"
#include "Rtsp.h"


class RtspConnection : public TcpConnection
{
public:
    using Ptr = std::shared_ptr<RtspConnection>;

    RtspConnection(std::shared_ptr<Rtsp> rtsp_server, TaskScheduler *task_scheduler, SOCKET sockfd);
    virtual ~RtspConnection();

    void OnMessage(BufferReader* buffer);
    void OnClose();
    bool isActive() const { return active_; } 
    
private:
    bool onRead(BufferReader& buffer);
    bool onWrite(BufferWirte& buffer);
    bool onClose();



private:
    bool active_ = false;
    std::atomic<int> heart_count_ = 0;
    std::shared_ptr<Rtsp> rtsp_server_;
};



#endif // _RTSPCONNECTION_H_