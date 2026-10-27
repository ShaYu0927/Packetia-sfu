#include "RtspSession.h"
#include "logger.h"
#include "MediaStreamAffinity.h"
#include "SfuEndpoint.h"
#include "RtcpContext.h"
#include "StreamAccessPolicy.h"
#include "RtspUtil.h"
#include "HttpUtil.h"
#include <algorithm>
#include <limits>
#include <unordered_set>
#include <vector>


namespace rtsp 
{
RtspSession::RtspSession(RtspConnection::Ptr conn, std::shared_ptr<service::auth::RtspDigestAuthenticator> digest)
    : task_scheduler_(conn->GetTaskScheduler()), conn_(std::move(conn)),
      rtsp_request_(std::make_unique<RtspRequest>()), digest_(std::move(digest))
{
    if (digest_) auth_ = std::make_unique<service::auth::AuthService>(digest_,
        std::make_shared<service::auth::StreamAccessPolicy>());
}

bool RtspSession::OnRead(TcpConnection::Ptr conn, BufferReader& buffer)
{
    constexpr size_t kByteBudget = 256 * 1024;
    constexpr size_t kFrameBudget = 512;

    size_t processedBytes = 0;
    size_t processedFrames = 0;
    
    while (true)
    {
        const size_t before = buffer.ReadableBytes();
        if (before == 0)
            return true;

        if (processedBytes >= kByteBudget || processedFrames >= kFrameBudget)
            return conn && conn->RequestReadContinuation();

        auto r = TryConsumeOneFrame(buffer);
        if (r == ParseResult::NEED_MORE) break;
        if (r == ParseResult::ERROR) return false;

        const size_t after = buffer.ReadableBytes();

        if (after >= before)
        {
            return false;
        }

        processedBytes += (before - after);
        processedFrames++;
    }
    return true;
}

void RtspSession::Start()
{
    LOG_INFO("RtspSession started for sockfd: " + std::to_string(conn_->GetSocket()));
}

void RtspSession::OnClosed(int reason)
{
    (void)reason;
    (void)lifecycle_.Dispatch(SessionEvent::Close);
    CloseMediaTransports();
    media_session_.reset();
    rtsp_request_.reset();
}

void RtspSession::SendRaw(std::string_view s,size_t size)
{
    if (!conn_) return;

    if (conn_->Send(s.data(), s.size()) != TcpConnection::SendResult::Queued)
    {
        LOG_ERROR("RTSP response could not be queued; closing connection");
        conn_->Disconnect();
    }
}

void RtspSession::OnInterleaved(int channel,const uint8_t*p, int len)
{
    if (channel < 0 || channel > 255 || !p || len <= 0)
    {
        LOG_ERROR("invalid interleaved packet, channel=", channel, " len=", len);
        return;
    }

    auto it = media_transports_.find(static_cast<uint8_t>(channel));
    if (it == media_transports_.end() || !it->second || !it->second->transport)
    {
        LOG_ERROR("transport binding not found, channel=", channel);
        return;
    }
    if (it->second->interleaved)
        (void)it->second->interleaved->InputInterleaved(static_cast<uint8_t>(channel), p, static_cast<size_t>(len), Timestamp::NowMs());
}

void RtspSession::Dispatch(const char* p, size_t total)
{
    if (CurrentState() == SessionState::Closed) return;
    RtspRequest::RtspRequestInfo req;
    if (!rtsp_request_->ParseRequest(p, total, req)) 
    {
        const auto response = RtspRequest::BuildStatusResponse(req.cseq, "400 Bad Request");
        SendRaw(response, response.size());
        return;
    }

    if (req.cseq < 0) 
    {
        return;
    }


    if (!AuthenticateRequest(req)) return;
    if (req.method == "OPTIONS")  { HandleCmdOptions(req);  return; }
    if (req.method == "DESCRIBE") { HandleCmdDescribe(req); return; }
    if (req.method ==  "ANNOUNCE"){ HandleCmdANNOUNCE(req); return; }
    if (req.method == "SETUP")    { HandleCmdSetup(req);    return; }
    if (req.method == "PLAY")     { HandleCmdPlay(req);     return; }
    if (req.method == "PAUSE")    { HandleCmdPause(req);    return; }
    if (req.method == "TEARDOWN") { HandleCmdTeardown(req); return; }
    if (req.method == "RECORD")   { HandleCmdRecord(req);   return; }
    const auto response = RtspRequest::BuildStatusResponse(req.cseq, "405 Method Not Allowed");
    SendRaw(response, response.size());
}

bool RtspSession::AuthenticateRequest(const RtspRequest::RtspRequestInfo& req)
{
    namespace auth = service::auth;
    if (!auth_ || req.method == "OPTIONS") return true;
    const auto status = [&](const char* value) {
        const auto response = RtspRequest::BuildStatusResponse(req.cseq, value);
        SendRaw(response, response.size()); return false;
    };
    try
    {
        const auto header = req.GetHeader("authorization");
        const auto verified = auth_->Authenticate(auth::AuthRequest{header, req.method, req.url}, auth::Audience::Rtsp);
        if (!verified.Succeeded())
        {
            if (verified.error == auth::AuthError::NotConfigured || verified.error == auth::AuthError::InternalError)
                return status("500 Internal Server Error");
            const auto response = "RTSP/1.0 401 Unauthorized\r\nCSeq: " + std::to_string(req.cseq) +
                "\r\nWWW-Authenticate: " + digest_->Challenge(verified.error == auth::AuthError::ExpiredToken) +
                "\r\nContent-Length: 0\r\n\r\n";
            SendRaw(response, response.size());
            return false;
        }
        const auto& caller = *verified.context;
        if (!authenticated_user_.empty() && authenticated_user_ != caller.subject_id) return status("403 Forbidden");
        const auto stream = RtspUtil::GetSuffixFromSetupUrl(req.url);
        if (!auth::IsValidStreamPath(stream)) return status("400 Bad Request");
        // 活跃会话不能借用同一 Session ID 操作另一条流。
        if (CurrentState() != SessionState::Initial && authorized_stream_ != stream) return status("403 Forbidden");
        auth::Action action;
        if (req.method == "ANNOUNCE" || req.method == "SETUP" || req.method == "RECORD" || req.method == "TEARDOWN")
            action = auth::Action::Publish; // 当前 SETUP 仅实现推流接收端。
        else if (req.method == "DESCRIBE" || req.method == "PLAY" || req.method == "PAUSE")
            action = auth::Action::Subscribe;
        else return status("405 Method Not Allowed");
        auth::ResourceContext resource;
        resource.type = auth::ResourceType::Stream; resource.id = stream; resource.stream_id = stream;
        if (!auth_->Authorize(caller, action, resource).Allowed()) return status("403 Forbidden");
        authenticated_user_ = caller.subject_id;
        authorized_stream_ = stream;
        return true;
    }
    catch (...) { return status("500 Internal Server Error"); }
}

RtspSession::ParseResult RtspSession::TryConsumeOneFrame(BufferReader &buffer)
{
    if (buffer.ReadableBytes() == 0)
        return ParseResult::NEED_MORE;

    const uint8_t* p = (const uint8_t*)buffer.Peek();
    if (p[0] == '$')
    {
        return TryConsumeInterleaved(buffer);
    }
    else
    {
        if (mode_ == RTSP_SERVER)
            return TryConsumeRtspRequest(buffer);
        else
            return TryConsumeRtspResponse(buffer);
    }
}

RtspSession::ParseResult RtspSession::TryConsumeInterleaved(BufferReader &buffer)
{
    size_t n = buffer.ReadableBytes();
    if (n == 0) return ParseResult::NEED_MORE;

    const uint8_t* p = reinterpret_cast<const uint8_t*>(buffer.Peek());
    if (p[0] != '$') return ParseResult::ERROR;

    if (n < 4) return ParseResult::NEED_MORE;

    uint8_t channel = p[1];
    uint16_t len = (uint16_t(p[2]) << 8) | uint16_t(p[3]);
    size_t total = 4u + size_t(len);

    constexpr size_t kMaxInterleaved = 2 * 1024 * 1024;
    if (total > kMaxInterleaved) return ParseResult::ERROR;

    if (n < total) return ParseResult::NEED_MORE;

    OnInterleaved(channel, p + 4, len); 

    buffer.Retrieve(total);
    return ParseResult::CONSUMED;
}

RtspSession::ParseResult RtspSession::TryConsumeRtspRequest(BufferReader &buffer)
{
    size_t n = buffer.ReadableBytes();
    if (n == 0) return ParseResult::NEED_MORE;

    const char* p = buffer.Peek();
    if (p[0] == '$') return ParseResult::ERROR;

    constexpr size_t kMaxHeader = 32 * 1024;
    if (n > kMaxHeader)
    {

    }

    int idx = utils::FindCrlfCrlf(p, n);
    if (idx < 0) 
    {
        if (n > kMaxHeader) return ParseResult::ERROR; 
        return ParseResult::NEED_MORE;
    }

    size_t headerLen = size_t(idx) + 4; 
    if (headerLen > kMaxHeader) return ParseResult::ERROR;

    size_t bodyLen = 0;
    if (!utils::ParseContentLengthFromHeader(p, headerLen, bodyLen)) return ParseResult::ERROR;

    constexpr size_t kMaxBody = 2 * 1024 * 1024;
    if (bodyLen > kMaxBody) return ParseResult::ERROR;

    size_t total = headerLen + bodyLen;
    if (n < total) return ParseResult::NEED_MORE;

    /* Handle protocol */
    Dispatch(buffer.Peek(), total);

    buffer.Retrieve(total);
    return ParseResult::CONSUMED;
}

RtspSession::ParseResult RtspSession::TryConsumeRtspResponse(BufferReader &buffer)
{
    size_t n = buffer.ReadableBytes();
    if (n == 0) return ParseResult::NEED_MORE;

    const char* p = buffer.Peek();
    if (p[0] == '$') return ParseResult::ERROR;

    constexpr size_t kMaxHeader = 32 * 1024;
    int idx = utils::FindCrlfCrlf(p, n);
    if (idx < 0)
    {
        if (n > kMaxHeader) return ParseResult::ERROR;
        return ParseResult::NEED_MORE;
    }

    size_t headerLen = size_t(idx) + 4;
    if (headerLen > kMaxHeader) return ParseResult::ERROR;

    size_t bodyLen = 0;
    if (!utils::ParseContentLengthFromHeader(p, headerLen, bodyLen)) return ParseResult::ERROR;

    constexpr size_t kMaxBody = 2 * 1024 * 1024;
    if (bodyLen > kMaxBody) return ParseResult::ERROR;

    size_t total = headerLen + bodyLen;
    if (n < total) return ParseResult::NEED_MORE;

    OnRtspResponse(p, total);

    buffer.Retrieve(total);
    return ParseResult::CONSUMED;
}

void RtspSession::OnRtspRequest(const char *p, size_t total)
{

}

void RtspSession::OnRtspResponse(const char *p, size_t total)
{

}

void RtspSession::HandleCmdOptions(RtspRequest::RtspRequestInfo& req)
{
    std::string res  = rtsp_request_->HandleCmdOptions(req);
    LOG_INFO("Handling OPTIONS request, response size: " + std::to_string(res.size()));
    this->SendRaw(res,(size_t)res.size());
}

void RtspSession::HandleCmdDescribe(RtspRequest::RtspRequestInfo& req)
{
    const auto response = RtspRequest::BuildStatusResponse(req.cseq, "501 Not Implemented");
    SendRaw(response, response.size());
}

bool RtspSession::RequireState(const RtspRequest::RtspRequestInfo& req,
                               std::initializer_list<SessionState> allowed)
{
    if (std::find(allowed.begin(), allowed.end(), CurrentState()) != allowed.end()) return true;
    const auto response = RtspRequest::BuildStatusResponse(req.cseq, "455 Method Not Valid in This State");
    SendRaw(response, response.size());
    return false;
}

bool RtspSession::HasMatchingSession(const RtspRequest::RtspRequestInfo& req) const
{
    auto id = req.GetHeader("session");
    id = id.substr(0, id.find(';'));
    return media_session_ && id == std::to_string(media_session_->GetId());
}

void RtspSession::HandleCmdANNOUNCE(RtspRequest::RtspRequestInfo& req)
{
    if (!RequireState(req, {SessionState::Initial, SessionState::Announced})) return;
    std::string res  = rtsp_request_->HandleCmdANNOUNCE(req);
    if (res.empty()) res = RtspRequest::BuildStatusResponse(req.cseq, "400 Bad Request");
    if (res.rfind("RTSP/1.0 200 ", 0) == 0) (void)lifecycle_.Dispatch(SessionEvent::AnnounceOk);
    this->SendRaw(res,(size_t)res.size());
}

void RtspSession::HandleCmdSetup(RtspRequest::RtspRequestInfo& req)
{
    if (!RequireState(req, {SessionState::Initial, SessionState::Announced, SessionState::Ready})) return;
    if (CurrentState() == SessionState::Ready && !req.GetHeader("session").empty() && !HasMatchingSession(req)) {
        const auto response = RtspRequest::BuildStatusResponse(req.cseq, "454 Session Not Found");
        SendRaw(response, response.size());
        return;
    }
    std::shared_ptr<TransportBinding> pending;
    RtspTransport negotiated;
    const auto response = rtsp_request_->HandleCmdSetup(req,
        [&](uint64_t endpoint_id, RtspTransport& transport) {
            pending = std::make_shared<TransportBinding>();
            pending->endpoint_id = endpoint_id;
            const auto description = rtsp_request_->GetMediaSession()->GetTrackDescription(req.GetControlFromUrl());
            if (!description) return false;
            pending->track_id = std::to_string(description->getTrackIndex());
            if (transport.lower_transport == "TCP") {
                const auto rtp_channel = static_cast<uint8_t>(transport.interleaved_rtp);
                const auto rtcp_channel = static_cast<uint8_t>(transport.interleaved_rtcp);
                if (media_transports_.count(rtp_channel) || media_transports_.count(rtcp_channel))
                    return false;
                pending->interleaved = std::make_shared<media::transport::RtspInterleavedTransport>(
                    endpoint_id, conn_, rtp_channel, rtcp_channel);
                pending->transport = pending->interleaved;
            } else {
                auto udp = media::transport::UdpMediaTransport::CreateUnicast(
                    endpoint_id, task_scheduler_->weak_from_this().lock(),
                    SocketUtil::GetSocketIp(conn_->GetSocket()), conn_->GetIp(),
                    static_cast<uint16_t>(transport.client_rtp_port),
                    static_cast<uint16_t>(transport.client_rtcp_port));
                if (!udp) return false;
                transport.server_rtp_port = udp->LocalPort(MediaPacketType::Rtp);
                transport.server_rtcp_port = udp->LocalPort(MediaPacketType::Rtcp);
                pending->transport = std::move(udp);
            }
            pending->ingress = std::make_shared<media::transport::MediaEndpointIngress>(endpoint_id, pending->track_id);
            pending->transport->SetPacketSink(pending->ingress);
            negotiated = transport;
            return true;
        });

    const auto endpoint_id = rtsp_request_->GetLastSetupEndpointId();
    if (endpoint_id && pending && pending->transport) {
        media_session_ = rtsp_request_->GetMediaSession();
        pending->session = media_session_;
        if (negotiated.lower_transport == "TCP") {
            media_transports_[static_cast<uint8_t>(negotiated.interleaved_rtp)] = pending;
            media_transports_[static_cast<uint8_t>(negotiated.interleaved_rtcp)] = pending;
        } else {
            udp_transports_.push_back(pending);
        }
        auto endpoint = std::dynamic_pointer_cast<media::SfuEndpoint>(
            utils::EndpointManager::Instance().Find(endpoint_id));
        if (endpoint) {
            std::weak_ptr<IMediaTransport> weak = pending->transport;
            endpoint->SetTrackRtcpSendCallback(pending->track_id, [weak](const uint8_t* data, size_t size) {
                auto transport = weak.lock();
                return transport && transport->SendRtcp(data, size) == SendResult::Ok;
            });
        }
        (void)lifecycle_.Dispatch(SessionEvent::SetupOk);
    }
    SendRaw(response, response.size());
}
void RtspSession::HandleCmdRecord(RtspRequest::RtspRequestInfo& req)
{
    if (!RequireState(req, {SessionState::Ready, SessionState::Recording})) return;
    if (!HasMatchingSession(req)) {
        const auto response = RtspRequest::BuildStatusResponse(req.cseq, "454 Session Not Found");
        SendRaw(response, response.size());
        return;
    }
    std::string res  = rtsp_request_->HandleCmdRecord(req);
    if (res.empty()) res = RtspRequest::BuildStatusResponse(req.cseq, "454 Session Not Found");
    if (res.rfind("RTSP/1.0 200 ", 0) == 0) (void)lifecycle_.Dispatch(SessionEvent::RecordOk);
    this->SendRaw(res,(size_t)res.size());
}
void RtspSession::HandleCmdPlay(RtspRequest::RtspRequestInfo& req)
{
    const auto response = RtspRequest::BuildStatusResponse(req.cseq, "501 Not Implemented");
    SendRaw(response, response.size());
}
void RtspSession::HandleCmdPause(RtspRequest::RtspRequestInfo& req)
{
    const auto response = RtspRequest::BuildStatusResponse(req.cseq, "501 Not Implemented");
    SendRaw(response, response.size());
}
void RtspSession::HandleCmdTeardown(RtspRequest::RtspRequestInfo& req)
{
    if (!HasMatchingSession(req)) {
        const auto response = RtspRequest::BuildStatusResponse(req.cseq, "454 Session Not Found");
        SendRaw(response, response.size());
        return;
    }
    if (!RequireState(req, {SessionState::Ready, SessionState::Recording})) return;
    CloseMediaTransports();
    media_session_.reset();
    rtsp_request_ = rtsp_request_->CreateNextPublication();
    (void)lifecycle_.Dispatch(SessionEvent::Teardown);
    const auto response = RtspRequest::BuildStatusResponse(req.cseq, "200 OK");
    SendRaw(response, response.size());
}

void RtspSession::CloseMediaTransports()
{
    std::unordered_set<uint64_t> closed;
    std::unordered_set<const TransportBinding*> released;
    auto release = [&](const std::shared_ptr<TransportBinding>& binding) {
        if (!binding || !released.insert(binding.get()).second) return;
        if (binding->transport) binding->transport->Close();
        if (!closed.insert(binding->endpoint_id).second) return;
        auto endpoint = std::dynamic_pointer_cast<media::SfuEndpoint>(
            utils::EndpointManager::Instance().Find(binding->endpoint_id));
        if (endpoint) {
            endpoint->SetRtcpSendCallback({});
            endpoint->Stop();
            utils::EndpointManager::Instance().Remove(binding->endpoint_id);
        }
        if (binding->session) binding->session->UnbindTrackEndpoint(binding->endpoint_id);
    };
    for (auto& entry : media_transports_) release(entry.second);
    for (auto& binding : udp_transports_) release(binding);
    media_transports_.clear();
    udp_transports_.clear();
}

}
