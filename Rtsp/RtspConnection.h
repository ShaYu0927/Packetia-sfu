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
#include "UdpServer.h"
#include "RtpInterleaved.h"
#include "PacketPool.h"
#include "RtpThreadPool.h"
#include "ShardedWorkerPool.h"

class RtspServer;

std::shared_ptr<RtpTrack> createTrack(
        TrackType type,
        const std::string& codec_name,
        int payload_type,
        uint32_t clock_rate,
        int track_index);


class RtspConnection : public TcpConnection , public UDPServer
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


    bool HandleRtspRequest(BufferReader& buffer);
    bool HandleRtspResponse(BufferReader& buffer);
    

    void HandleCmdOptions();
    void HandleCmdDescribe();
    void HandleCmdANNOUNCE();
    void HandleCmdSetup();
    void HandleCmdRecord();
    void HandleCmdPlay();
    void HandleCmdPause();
    void HandleCmdTeardown();

    void SendRtspMessage(std::shared_ptr<char> data, uint32_t size);

    int RtspConn_ConsumeInterleaved(BufferReader& buffer);

protected:
    friend class RtspServer; 
    friend class RtspMessage;
    friend class RtpConnection;


    bool onRead(BufferReader& buffer);
    bool onWrite(BufferWirte& buffer);
    bool onClose();

private:
    int ParseStreamId(const std::string& control);

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
    MediaSessionId session_id_ = 0;

    std::shared_ptr<RtpConnection> rtp_connection_;
    std::shared_ptr<Rtsp> rtsp_;
    std::unique_ptr<Sdp> sdp_;
    std::unique_ptr<RtspRequest> rtsp_request_;
    std::unique_ptr<RtspResponse> read_buffer_;


    std::shared_ptr<Channel>       rtp_channel_;                        // rtp socket
	std::shared_ptr<Channel>       rtcp_channels_[MAX_MEDIA_CHANNEL];   //rtcp socket

    RtpInterleaved interleaved_;
    std::unique_ptr<PacketPool> packet_pool_;
    ShardedWorkerPool* media_pool_ = nullptr;    

};



#endif // _RTSPCONNECTION_H_