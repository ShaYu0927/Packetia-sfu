#ifndef _RTSPCONNECTION_H_
#define _RTSPCONNECTION_H_

#include "TcpConnection.h"
#include "BufferWrite.h"
#include "RtpConnection.h"
#include "EventLoop.h"

class RtspServer;

class RtspConnection : public TcpConnection
{
public:
    using Ptr = std::shared_ptr<RtspConnection>;

    static std::shared_ptr<RtspConnection> Create(std::shared_ptr<RtspServer> rtsp_server,
                                                  TaskScheduler* task_scheduler,
                                                  SOCKET sockfd);

    enum ConnectionMode
    {
        RTSP_SERVER, 
		RTSP_PUSHER,
    };

    enum ConnectionState
    {
        INIT,
        CONNECTING,
        CONNECTED,
        DISCONNECTED,
        START_PLAY,
		START_PUSH
    };


    virtual ~RtspConnection();
    bool isActive() const { return active_; } 
    void SendRtspMessage(std::shared_ptr<char> data, uint32_t size);
protected:
    bool onRead(BufferReader& buffer);
    bool onWrite(BufferWirte& buffer);

private:

    RtspConnection(std::shared_ptr<RtspServer> rtsp_server,
                   TaskScheduler* task_scheduler,
                   SOCKET sockfd);


private:
    bool active_ = false;
    std::shared_ptr<RtspServer> rtsp_server_;
    TaskScheduler *task_scheduler_;
    ConnectionMode mode_ = RTSP_SERVER;
    ConnectionState state_ = INIT;
};



#endif // _RTSPCONNECTION_H_