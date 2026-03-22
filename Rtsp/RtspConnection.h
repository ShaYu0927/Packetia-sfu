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
    void InitCallbacks();

    ConnectionType GetConnectionType() const override { return ConnectionType::Rtsp; }

    void OnMessage(BufferReader* buffer);
    bool isActive() const { return active_; } 


    void SendRtspMessage(std::shared_ptr<char> data, uint32_t size);

    int RtspConn_ConsumeInterleaved(BufferReader& buffer);

protected:
    bool onRead(BufferReader& buffer);
    bool onWrite(BufferWirte& buffer);

private:

    RtspConnection(std::shared_ptr<RtspServer> rtsp_server,
                   TaskScheduler* task_scheduler,
                   SOCKET sockfd);


private:
    bool active_ = false;
    std::atomic<int> heart_count_ = 0;
    std::shared_ptr<RtspServer> rtsp_server_;
    TaskScheduler *task_scheduler_;
    ConnectionMode mode_ = RTSP_SERVER;
    ConnectionState state_ = INIT;


    std::shared_ptr<Channel>       rtp_channel_;                        // rtp socket
	std::shared_ptr<Channel>       rtcp_channels_[2];   //rtcp socket
};



#endif // _RTSPCONNECTION_H_