#ifndef _RTSPSERSSION_H_
#define _RTSPSERSSION_H_

#include "TcpSession.h"
#include "RtspServer.h"

namespace rtsp 
{

class RtspSession : public itcp_sess::ISessionBase,
                    public std::enable_shared_from_this<RtspSession>
{
public:
    using Ptr = std::shared_ptr<RtspSession>;

    RtspSession(std::shared_ptr<RtspServer> rtsp_server,
                TaskScheduler* task_scheduler,
                TcpConnection::Ptr conn)
        : rtsp_server_(std::move(rtsp_server))
        , task_scheduler_(task_scheduler)
        , conn_(std::move(conn))
        , rtsp_request_(std::make_unique<RtspRequest>())
        , sdp_(std::make_unique<Sdp>())
        , rtsp_(std::make_shared<Rtsp>())
        , packet_pool_(std::make_unique<PacketPool>(2048, 1000))
    {
        interleaved_.SetPacketPool(packet_pool_.get());

        auto rtp_handler = std::make_shared<RtpJobHandler>(packet_pool_.get());
        media_pool_ = WorkerService::get_pool("media");
        if (!media_pool_)
        {
            WorkerService::create_pool("media", 4, rtp_handler, 4096,
                ShardedWorkerPool::DropPolicy::DropHead);
            media_pool_ = WorkerService::get_pool("media");
        }
    }

protected:
    bool OnRead(TcpConnection::Ptr conn, BufferReader& buffer) override;
    void OnClosed(int reason) override;
    void Start() override;

private:
    std::shared_ptr<RtspServer> rtsp_server_;
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
};
}

#endif /* _RTSPSERSSION_H_ */