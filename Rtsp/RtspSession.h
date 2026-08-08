#ifndef _RTSPSERSSION_H_
#define _RTSPSERSSION_H_

#include "TcpSession.h"
#include "RtspMessage.h"
#include "ShardedWorkerPool.h"
#include "RtspConnection.h"
#include "RtspInterleavedTransport.h"
#include "MediaEndpointIngress.h"
#include <cstddef>
#include "Sdp.h"
#include <unordered_map>

namespace rtsp 
{

class RtspSession : public itcp_sess::ISessionBase,
                    public std::enable_shared_from_this<RtspSession>
{
public:
    using Ptr = std::shared_ptr<RtspSession>;

    enum class ParseResult
    {
        CONSUMED,   
        NEED_MORE,  
        ERROR   
    };

    enum SessionMode
    {
        RTSP_SERVER, 
		RTSP_PUSHER,
    };

    enum SessionState
    {
        INIT,
        CONNECTING,
        CONNECTED,
        DISCONNECTED,
        START_PLAY,
		START_PUSH
    };

    explicit RtspSession(RtspConnection::Ptr conn)
        : conn_(std::move(conn))
        , rtsp_request_(std::make_unique<RtspRequest>())
    {
        task_scheduler_ = conn_->GetTaskScheduler();
    }

    ~RtspSession()
    {
        LOG_INFO("RtspSession destroyed, fd=" + std::to_string(conn_ ? conn_->GetSocket() : -1));
    }

    void SendRaw(std::string_view s,size_t size);
    void OnInterleaved(int channel,const uint8_t*p, int len);
    void Dispatch(const char* p, size_t total);

    ParseResult TryConsumeOneFrame(BufferReader& buffer);
    ParseResult TryConsumeInterleaved(BufferReader &buffer);
    ParseResult TryConsumeRtspRequest(BufferReader &buffer);
    ParseResult TryConsumeRtspResponse(BufferReader &buffer);



    void OnRtspRequest(const char*p, size_t total);
    void OnRtspResponse(const char*p, size_t total);

public:
    void HandleCmdOptions(RtspRequest::RtspRequestInfo& req);
    void HandleCmdDescribe(RtspRequest::RtspRequestInfo& req);
    void HandleCmdANNOUNCE(RtspRequest::RtspRequestInfo& req);
    void HandleCmdSetup(RtspRequest::RtspRequestInfo& req);
    void HandleCmdRecord(RtspRequest::RtspRequestInfo& req);
    void HandleCmdPlay(RtspRequest::RtspRequestInfo& req);
    void HandleCmdPause(RtspRequest::RtspRequestInfo& req);
    void HandleCmdTeardown(RtspRequest::RtspRequestInfo& req);


public:
    bool OnRead(TcpConnection::Ptr conn, BufferReader& buffer) override;
    void OnClosed(int reason) override;
    void Start() override;

private:
    struct TransportBinding
    {
        std::shared_ptr<media::transport::RtspInterleavedTransport> transport;
        std::shared_ptr<media::transport::MediaEndpointIngress> ingress;
    };

    void CloseMediaTransports();

    TaskScheduler* task_scheduler_;
    RtspConnection::Ptr conn_;
    std::unique_ptr<RtspRequest> rtsp_request_;
    std::unique_ptr<sdp::Sdp> sdp_;
    std::unique_ptr<PacketPool> packet_pool_;
    ShardedWorkerPool* media_pool_ = nullptr;
    int session_id_{0};

    SessionMode mode_ = RTSP_SERVER;
    SessionState state_ = INIT;
    MediaSession::Ptr media_session_;
    std::unordered_map<uint8_t, std::shared_ptr<TransportBinding>>
        media_transports_;
};
}

#endif /* _RTSPSERSSION_H_ */
