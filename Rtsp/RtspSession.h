#ifndef _RTSPSERSSION_H_
#define _RTSPSERSSION_H_

#include "TcpSession.h"
#include "RtspMessage.h"
#include "ShardedWorkerPool.h"
#include "RtspConnection.h"
#include "RtspInterleavedTransport.h"
#include "MediaEndpointIngress.h"
#include "UdpMediaTransport.h"
#include <cstddef>
#include "Sdp.h"
#include "StateController.h"
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

    enum class SessionState { Initial, Announced, Ready, Recording, Closed };
    SessionState CurrentState() const noexcept { return lifecycle_.CurrentState(); }

    explicit RtspSession(RtspConnection::Ptr conn)
        : conn_(std::move(conn))
        , rtsp_request_(std::make_unique<RtspRequest>())
    {
        task_scheduler_ = conn_->GetTaskScheduler();
    }

    ~RtspSession()
    {
        (void)lifecycle_.Dispatch(SessionEvent::Close);
        CloseMediaTransports();
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
    enum class SessionEvent { AnnounceOk, SetupOk, RecordOk, Teardown, Close };
    using Lifecycle = utils::StateController<SessionState, SessionEvent, RtspSession>;
    bool RequireState(const RtspRequest::RtspRequestInfo& req,
                      std::initializer_list<SessionState> allowed);
    bool HasMatchingSession(const RtspRequest::RtspRequestInfo& req) const;
    struct TransportBinding
    {
        ~TransportBinding() { if (transport) transport->Close(); }
        uint64_t endpoint_id = 0;
        std::string track_id;
        std::shared_ptr<IMediaTransport> transport;
        std::shared_ptr<media::transport::RtspInterleavedTransport> interleaved;
        std::shared_ptr<media::transport::MediaEndpointIngress> ingress;
        MediaSession::Ptr session;
    };

    void CloseMediaTransports();

    TaskScheduler* task_scheduler_;
    RtspConnection::Ptr conn_;
    std::unique_ptr<RtspRequest> rtsp_request_;
    std::unique_ptr<sdp::Sdp> sdp_;
    ShardedWorkerPool* media_pool_ = nullptr;
    int session_id_{0};

    SessionMode mode_ = RTSP_SERVER;
    Lifecycle lifecycle_{*this, SessionState::Initial, {
        Lifecycle::On(SessionState::Initial, SessionEvent::AnnounceOk, SessionState::Announced),
        Lifecycle::Stay(SessionState::Announced, SessionEvent::AnnounceOk),
        // SETUP may also use a media description registered before this connection.
        Lifecycle::On(SessionState::Initial, SessionEvent::SetupOk, SessionState::Ready),
        Lifecycle::On(SessionState::Announced, SessionEvent::SetupOk, SessionState::Ready),
        Lifecycle::Stay(SessionState::Ready, SessionEvent::SetupOk),
        Lifecycle::On(SessionState::Ready, SessionEvent::RecordOk, SessionState::Recording),
        Lifecycle::Stay(SessionState::Recording, SessionEvent::RecordOk),
        Lifecycle::On(SessionState::Ready, SessionEvent::Teardown, SessionState::Initial),
        Lifecycle::On(SessionState::Recording, SessionEvent::Teardown, SessionState::Initial),
        Lifecycle::On(SessionState::Initial, SessionEvent::Close, SessionState::Closed),
        Lifecycle::On(SessionState::Announced, SessionEvent::Close, SessionState::Closed),
        Lifecycle::On(SessionState::Ready, SessionEvent::Close, SessionState::Closed),
        Lifecycle::On(SessionState::Recording, SessionEvent::Close, SessionState::Closed),
        Lifecycle::Stay(SessionState::Closed, SessionEvent::Close)
    }};
    MediaSession::Ptr media_session_;
    std::unordered_map<uint8_t, std::shared_ptr<TransportBinding>>
        media_transports_;
    std::vector<std::shared_ptr<TransportBinding>> udp_transports_;
};
}

#endif /* _RTSPSERSSION_H_ */
