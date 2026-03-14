#ifndef _RTSPSERSSION_H_
#define _RTSPSERSSION_H_

#include "TcpSession.h"
#include "RtspMessage.h"
#include "ShardedWorkerPool.h"
#include "RtpInterleaved.h"
#include "RtspConnection.h"
#include <cstddef>
#include "Sdp.h"

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
        , packet_pool_(std::make_unique<PacketPool>(2048, 1000))
    {
        interleaved_.SetPacketPool(packet_pool_.get());
        task_scheduler_ = conn_->GetTaskScheduler();
    }

    void SendRaw(std::string_view s,size_t size);

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


protected:
    bool OnRead(TcpConnection::Ptr conn, BufferReader& buffer) override;
    void OnClosed(int reason) override;
    void Start() override;

private:
    TaskScheduler* task_scheduler_;
    RtspConnection::Ptr conn_;
    std::unique_ptr<RtspRequest> rtsp_request_;
    std::unique_ptr<sdp::Sdp> sdp_;
    std::unique_ptr<PacketPool> packet_pool_;
    ShardedWorkerPool* media_pool_ = nullptr;
    RtpInterleaved interleaved_;
    int session_id_{0};

    SessionMode mode_ = RTSP_SERVER;
    SessionState state_ = INIT;
};
}

#endif /* _RTSPSERSSION_H_ */