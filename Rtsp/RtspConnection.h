#ifndef _RTSPCONNECTION_H_
#define _RTSPCONNECTION_H_

#include "TcpConnection.h"
#include "BufferWrite.h"
#include "EventLoop.h"
#include "Rtsp.h"
#include "Media.h"
#include "logger.h"

class RtspServer;


class RtspConnection : public TcpConnection
{
public:
    using Ptr = std::shared_ptr<RtspConnection>;

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


    RtspConnection(std::shared_ptr<RtspServer> rtsp_server, TaskScheduler *task_scheduler, SOCKET sockfd);
    virtual ~RtspConnection();

    ConnectionType GetConnectionType() const override { return ConnectionType::Rtsp; }

    void OnMessage(BufferReader* buffer);
    void OnClose();
    bool isActive() const { return active_; } 


    bool HandleRtspRequest(BufferReader& buffer);
    bool HandleRtspResponse(BufferReader& buffer);
    
protected:
    friend class RtspServer; 
    bool onRead(BufferReader& buffer);
    bool onWrite(BufferWirte& buffer);
    bool onClose();



private:
    bool active_ = false;
    std::atomic<int> heart_count_ = 0;
    std::shared_ptr<RtspServer> rtsp_server_;
    TaskScheduler *task_scheduler_;
    ConnectionMode mode_ = RTSP_SERVER;
    ConnectionState state_ = INIT;
    MediaSessionId session_id_ = 0;
};



#endif // _RTSPCONNECTION_H_