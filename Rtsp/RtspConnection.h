#ifndef _RTSPCONNECTION_H_
#define _RTSPCONNECTION_H_

#include "TcpConnection.h"
#include "BufferWrite.h"
#include "RtspMessage.h"
#include "MediaSession.h"
#include "RtpConnection.h"
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
    bool isActive() const { return active_; } 


    bool HandleRtspRequest(BufferReader& buffer);
    bool HandleRtspResponse(BufferReader& buffer);
    

    void HandleCmdOptions();
    void HandleCmdDescribe();
    void HandleCmdSetup();
    void HandleCmdPlay();
    void HandleCmdPause();
    void HandleCmdTeardown();

    void SendRtspMessage(std::shared_ptr<char> data, uint32_t size);

protected:
    friend class RtspServer; 
    friend class RtspMessage;
    friend class RtpConnection;


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

    std::shared_ptr<RtpConnection> rtp_connection_;
    std::shared_ptr<Rtsp> rtsp_;
    std::unique_ptr<Sdp> sdp_;
    std::unique_ptr<RtspRequest> rtsp_request_;
    std::unique_ptr<RtspResponse> read_buffer_;


    std::shared_ptr<Channel>       rtp_channel_;
	std::shared_ptr<Channel>       rtcp_channels_[MAX_MEDIA_CHANNEL];
};



#endif // _RTSPCONNECTION_H_