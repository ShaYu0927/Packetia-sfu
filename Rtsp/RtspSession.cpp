#include "RtspSession.h"
#include "logger.h"
#include "MediaStreamAffinity.h"
#include "MediaEndpoint.h"
#include "RtcpContext.h"
#include <algorithm>
#include <iomanip>
#include <limits>
#include <unordered_set>
#include <vector>


namespace rtsp 
{
static inline int FindCrlfCrlf(const char* p, size_t n)
{
    for (size_t i = 0; i + 3 < n; ++i) 
    {
        if (p[i] == '\r' && p[i+1] == '\n' && p[i+2] == '\r' && p[i+3] == '\n')
            return (int)i;
    }
    return -1;
}

static inline bool IStartsWith(const char* s, size_t n, const char* prefix)
{
    size_t m = 0;
    while (prefix[m]) m++;
    if (n < m) return false;
    for (size_t i = 0; i < m; ++i) {
        unsigned char a = (unsigned char)s[i];
        unsigned char b = (unsigned char)prefix[i];
        if (a >= 'A' && a <= 'Z') a = a - 'A' + 'a';
        if (b >= 'A' && b <= 'Z') b = b - 'A' + 'a';
        if (a != b) return false;
    }
    return true;
}

static inline bool ParseContentLengthFromHeader(const char* header, size_t headerLen, size_t& outLen)
{
    outLen = 0;

    size_t i = 0;
    while (i < headerLen) 
    {
        size_t lineEnd = i;
        while (lineEnd + 1 < headerLen && !(header[lineEnd] == '\r' && header[lineEnd + 1] == '\n'))
            lineEnd++;
        size_t lineLen = (lineEnd > i) ? (lineEnd - i) : 0;

        if (lineLen == 0) break; 

        const char* line = header + i;

        if (IStartsWith(line, lineLen, "content-length:")) 
        {
            size_t k = 15;
            while (k < lineLen && (line[k] == ' ' || line[k] == '\t')) k++;

            size_t v = 0;
            bool any = false;
            while (k < lineLen) 
            {
                char c = line[k];
                if (c < '0' || c > '9') break;
                any = true;
                v = v * 10 + size_t(c - '0');
                k++;
            }
            if (!any) return false;
            outLen = v;
            return true;
        }

        i = lineEnd + 2; 
    }

    return true; 
}

static void DumpBytes(const uint8_t* data, size_t len, size_t max_dump = 16)
{
    if (!data)
        return;

    std::ostringstream oss;
    size_t n = std::min(len, max_dump);
    for (size_t i = 0; i < n; ++i)
    {
        oss << std::hex << std::setw(2) << std::setfill('0') << static_cast<int>(data[i]) << " ";
    }
    LOG_INFO("RtspSession dump len=", len, " bytes=", oss.str());
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
    CloseMediaTransports();
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
    RtspRequest::RtspRequestInfo req;
    if (!rtsp_request_->ParseRequest(p, total, req)) 
    {
        return;
    }

    if (req.cseq < 0) 
    {
        return;
    }


    if (req.method == "OPTIONS")  { HandleCmdOptions(req);  return; }
    if (req.method == "DESCRIBE") { HandleCmdDescribe(req); return; }
    if (req.method ==  "ANNOUNCE"){ HandleCmdANNOUNCE(req); return; }
    if (req.method == "SETUP")    { HandleCmdSetup(req);    return; }
    if (req.method == "PLAY")     { HandleCmdPlay(req);     return; }
    if (req.method == "PAUSE")    { HandleCmdPause(req);    return; }
    if (req.method == "TEARDOWN") { HandleCmdTeardown(req); return; }
    if (req.method == "RECORD")   { HandleCmdRecord(req);   return; }
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

    int idx = FindCrlfCrlf(p, n);
    if (idx < 0) 
    {
        if (n > kMaxHeader) return ParseResult::ERROR; 
        return ParseResult::NEED_MORE;
    }

    size_t headerLen = size_t(idx) + 4; 
    if (headerLen > kMaxHeader) return ParseResult::ERROR;

    size_t bodyLen = 0;
    if (!ParseContentLengthFromHeader(p, headerLen, bodyLen)) return ParseResult::ERROR;

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
    int idx = FindCrlfCrlf(p, n);
    if (idx < 0)
    {
        if (n > kMaxHeader) return ParseResult::ERROR;
        return ParseResult::NEED_MORE;
    }

    size_t headerLen = size_t(idx) + 4;
    if (headerLen > kMaxHeader) return ParseResult::ERROR;

    size_t bodyLen = 0;
    if (!ParseContentLengthFromHeader(p, headerLen, bodyLen)) return ParseResult::ERROR;

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
}
void RtspSession::HandleCmdANNOUNCE(RtspRequest::RtspRequestInfo& req)
{
    std::string res  = rtsp_request_->HandleCmdANNOUNCE(req);
    this->SendRaw(res,(size_t)res.size());
}

void RtspSession::HandleCmdSetup(RtspRequest::RtspRequestInfo& req)
{
    std::shared_ptr<TransportBinding> pending;
    RtspTransport negotiated;
    const auto response = rtsp_request_->HandleCmdSetup(req,
        [&](uint64_t endpoint_id, RtspTransport& transport) {
            pending = std::make_shared<TransportBinding>();
            pending->endpoint_id = endpoint_id;
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
            pending->ingress = std::make_shared<media::transport::MediaEndpointIngress>(endpoint_id);
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
            endpoint->SetRtcpSendCallback([weak](const uint8_t* data, size_t size) {
                auto transport = weak.lock();
                return transport && transport->SendRtcp(data, size) == SendResult::Ok;
            });
        }
    }
    SendRaw(response, response.size());
}
void RtspSession::HandleCmdRecord(RtspRequest::RtspRequestInfo& req)
{
    std::string res  = rtsp_request_->HandleCmdRecord(req);
    this->SendRaw(res,(size_t)res.size());
}
void RtspSession::HandleCmdPlay(RtspRequest::RtspRequestInfo& req)
{

}
void RtspSession::HandleCmdPause(RtspRequest::RtspRequestInfo& req)
{
}
void RtspSession::HandleCmdTeardown(RtspRequest::RtspRequestInfo& req)
{
    auto id = req.GetHeader("session");
    id = id.substr(0, id.find(';'));
    if (!media_session_ || id != std::to_string(media_session_->GetId())) {
        const auto response = RtspRequest::BuildStatusResponse(req.cseq, "454 Session Not Found");
        SendRaw(response, response.size());
        return;
    }
    CloseMediaTransports();
    const auto response = RtspRequest::BuildStatusResponse(req.cseq, "200 OK");
    SendRaw(response, response.size());
}

void RtspSession::CloseMediaTransports()
{
    std::unordered_set<uint64_t> closed;
    auto release = [&](const std::shared_ptr<TransportBinding>& binding) {
        if (!binding || !closed.insert(binding->endpoint_id).second) return;
        if (binding->transport) binding->transport->Close();
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
