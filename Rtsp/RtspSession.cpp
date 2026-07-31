#include "RtspSession.h"
#include "logger.h"
#include "MediaStreamAffinity.h"
#include "MediaEndpoint.h"
#include <algorithm>
#include <iomanip>
#include <limits>
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
    
    size_t readable = buffer.ReadableBytes();

    while (buffer.ReadableBytes() > 0)
    {
        if (processedBytes >= kByteBudget || processedFrames >= kFrameBudget)
            break;

        size_t before = buffer.ReadableBytes();
        auto r = TryConsumeOneFrame(buffer);
        if (r == ParseResult::NEED_MORE) break;
        if (r == ParseResult::ERROR) return false;

        size_t after = buffer.ReadableBytes();

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
    
}

void RtspSession::SendRaw(std::string_view s,size_t size)
{
    if (!conn_) return;

    conn_->Send(s.data(), s.size());
}

void RtspSession::OnInterleaved(int channel,const uint8_t*p, int len)
{
    if (channel < 0 || channel > 255 || !p || len <= 0)
    {
        LOG_ERROR("invalid interleaved packet, channel=", channel, " len=", len);
        return;
    }

    auto media = media_session_;
    if (!media)
    {
        LOG_ERROR("invalid media session");
        return;
    }

    MediaSession::ChannelBinding binding;
    if (!media->GetChannelBinding(static_cast<uint8_t>(channel), &binding))
    {
        LOG_ERROR("channel binding not found, channel=", channel,
                  " session_id=", media->GetId());
        return;
    }

    if (!binding.valid)
    {
        LOG_ERROR("channel binding invalid, channel=", channel,
                  " session_id=", media->GetId());
        return;
    }


    WorkJob job{};
    job.target_id = binding.endpoint_id;
    job.type = binding.is_rtcp ? WorkType::Rtcp : WorkType::Rtp;
    uint32_t media_ssrc = 0;
    const bool has_media_ssrc = binding.is_rtcp
        ? media_affinity::TryGetRtcpMediaSsrc(
              reinterpret_cast<const uint8_t*>(p), static_cast<size_t>(len),
              media_ssrc)
        : media_affinity::TryGetRtpSsrc(
              reinterpret_cast<const uint8_t*>(p), static_cast<size_t>(len),
              media_ssrc);
    job.key = has_media_ssrc
        ? media_affinity::MakeStreamHandle(binding.endpoint_id, media_ssrc).affinity_key
        : binding.endpoint_id;
    job.raw.data = new uint8_t[len];
    std::memcpy(job.raw.data, p, static_cast<size_t>(len));
    job.raw.len = static_cast<uint32_t>(len);
    job.enqueue_ts = Timestamp::NowMs();
    job.deleter = [](WorkJob& job) {
        delete[] job.raw.data;
        job.raw.data = nullptr;
        job.raw.len = 0;
    };

    int ret = WorkerService::post("media",std::move(job));
    (void)ret;

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
    std::string res  = rtsp_request_->HandleCmdSetup(req);
    media_session_ = rtsp_request_->GetMediaSession();

    const uint64_t endpoint_id = rtsp_request_->GetLastSetupEndpointId();
    if (endpoint_id != 0)
    {
        auto endpoint = std::dynamic_pointer_cast<media::SfuEndpoint>(
            utils::EndpointManager::Instance().Find(endpoint_id));
        if (endpoint)
        {
            const uint8_t rtcp_channel = rtsp_request_->GetLastSetupRtcpChannel();
            std::weak_ptr<RtspConnection> weak_conn = conn_;
            endpoint->SetRtcpSendCallback(
                [weak_conn, rtcp_channel](const uint8_t* data, size_t len) -> bool {
                    if (!data || len == 0 || len > std::numeric_limits<uint16_t>::max())
                    {
                        return false;
                    }

                    auto conn = weak_conn.lock();
                    if (!conn || conn->IsClosed())
                    {
                        return false;
                    }
                    TaskScheduler* scheduler = conn->GetTaskScheduler();
                    if (!scheduler)
                    {
                        return false;
                    }

                    std::vector<uint8_t> frame(4 + len);
                    frame[0] = '$';
                    frame[1] = rtcp_channel;
                    frame[2] = static_cast<uint8_t>((len >> 8) & 0xFF);
                    frame[3] = static_cast<uint8_t>(len & 0xFF);
                    std::copy(data, data + len, frame.begin() + 4);

                    return scheduler->Post([weak_conn, frame = std::move(frame)] {
                        if (auto conn = weak_conn.lock())
                        {
                            conn->Send(reinterpret_cast<const char*>(frame.data()),
                                       static_cast<uint32_t>(frame.size()));
                        }
                    });
                });
        }
    }
    this->SendRaw(res,(size_t)res.size());
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
}
}
