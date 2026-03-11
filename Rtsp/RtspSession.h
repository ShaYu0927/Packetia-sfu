#ifndef _RTSPSERSSION_H_
#define _RTSPSERSSION_H_

#include "TcpSession.h"
#include "RtspServer.h"
#include "RtspMessage.h"

#include <cstddef>

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

    explicit RtspSession(TcpConnection::Ptr conn)
        : conn_(std::move(conn))
        , rtsp_request_(std::make_unique<RtspRequest>())
        , sdp_(std::make_unique<Sdp>())
        , rtsp_(std::make_shared<Rtsp>())
        , packet_pool_(std::make_unique<PacketPool>(2048, 1000))
    {
        interleaved_.SetPacketPool(packet_pool_.get());
        task_scheduler_ = conn_->GetTaskScheduler();

        auto rtp_handler = std::make_shared<RtpJobHandler>(packet_pool_.get());
        media_pool_ = WorkerService::get_pool("media");
        if (!media_pool_)
        {
            WorkerService::create_pool("media", 4, rtp_handler, 4096,
                ShardedWorkerPool::DropPolicy::DropHead);
            media_pool_ = WorkerService::get_pool("media");
        }
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
    TcpConnection::Ptr conn_;
    std::unique_ptr<RtspRequest> rtsp_request_;
    std::unique_ptr<Sdp> sdp_;
    std::shared_ptr<Rtsp> rtsp_;
    std::unique_ptr<PacketPool> packet_pool_;
    ShardedWorkerPool* media_pool_ = nullptr;
    RtpInterleaved interleaved_;
    std::shared_ptr<RtpConnection> rtp_connection_;
    int session_id_{0};

    ConnectionMode mode_ = RTSP_SERVER;
    ConnectionState state_ = INIT;

public:
    
    

};
}

#endif /* _RTSPSERSSION_H_ */