#include "RtspSession.h"
namespace rtsp 
{
static inline int FindCrlfCrlf(const char* p, size_t n)
{
    for (size_t i = 0; i + 3 < n; ++i) {
        if (p[i] == '\r' && p[i+1] == '\n' && p[i+2] == '\r' && p[i+3] == '\n')
            return (int)i;
    }
    return -1;
}

static inline bool IStartsWith(const char* s, size_t n, const char* prefix)
{
    // case-insensitive startswith for ASCII
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

        i = lineEnd + 2; // skip \r\n
    }

    return true; // 没有 Content-Length 就当 0
}


bool RtspSession::OnRead(TcpConnection::Ptr conn, BufferReader& buffer)
{
    constexpr size_t kByteBudget = 256 * 1024;
    constexpr size_t kFrameBudget = 512;

    size_t readable = buffer.ReadableBytes();

    LOG_INFO("[RTSP] OnRead fd=" 
             + std::to_string(conn->GetSocket()) 
             + " readable=" 
             + std::to_string(readable));

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

bool RtspSession::BindTrackByControl(std::string_view control, const std::shared_ptr<MediaSession> &media_session, std::shared_ptr<RtpTrack> &out_track)
{
    if (!rtsp_request_ || !rtsp_request_->sdp_ || !media_session) return false;

    for (const auto& m : rtsp_request_->sdp_->media_list_)
    {
        if (m.control != control) continue;

        auto tid = rtsp::RtspUtil::ParseStreamId(m.control); // optional<int>
        if (!tid) return false;

        int trackIdx = *tid;

        TrackType type;
        if (m.media_type == "video") type = TrackType::TrackVideo;
        else if (m.media_type == "audio") type = TrackType::TrackAudio;
        else return false;

        auto track_ptr = createTrack(type, m.codec_name, m.payload_type, m.clock_rate, trackIdx);
        if (!track_ptr) return false;

        media_session->AddTrack(type, m.codec_name, m.control, m.payload_type, m.clock_rate);
        MediaSessionManager::Instance().AddTrackChannel(trackIdx, track_ptr);
        media_session->BindRtpTrack(trackIdx, track_ptr);

        out_track = track_ptr;
        return true;
    }
    return false;
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


    if (req.method == "OPTIONS")  { HandleCmdOptions();  return; }
    if (req.method == "DESCRIBE") { HandleCmdDescribe(); return; }
    if (req.method == "SETUP")    { HandleCmdSetup();    return; }
    if (req.method == "PLAY")     { HandleCmdPlay();     return; }
    if (req.method == "PAUSE")    { HandleCmdPause();    return; }
    if (req.method == "TEARDOWN") { HandleCmdTeardown(); return; }
    if (req.method == "RECORD")   { HandleCmdRecord();   return; }
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

    // OnInterleaved(channel, p + 4, len); 

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

void RtspSession::HandleCmdOptions()
{
    std::shared_ptr<char> res(new char[2048], std::default_delete<char[]>());
    int size = rtsp_request_->BuildOptionsRes(res, 1024);
    LOG_INFO("Handling OPTIONS request, response size: " + std::to_string(size));
    this->SendRaw(res.get(),(size_t)size);

}

void RtspSession::HandleCmdDescribe()
{
}
void RtspSession::HandleCmdANNOUNCE()
{
    if(!rtsp_request_->sdp_)
    {
        rtsp_request_->sdp_ = std::make_shared<Sdp>();
    }
    std::string body = rtsp_request_->sdp_->buildANNOUNCEBody();
    if(body.size() == 0)
    {
        return;
    }

    LOG_INFO("Handling ANNOUNCE request");
    std::shared_ptr<char> res(new char[4096], std::default_delete<char[]>());
    int MessageSize = rtsp_request_->BuildANNOUNCERes(res, 4096);
    this->SendRaw(res.get(),(size_t)MessageSize);
}
void RtspSession::HandleCmdSetup()
{
    if (!rtsp_) { LOG_ERROR("RTSP context is null"); return; }

    std::string url = rtsp_request_->GetRtspUSuffix();
    auto controlTdx = rtsp_request_->GetControl();

    auto media_session = MediaSessionManager::Instance().GetSessionBySuffix(url);
    if (!media_session) 
    {
        media_session = MediaSession::CreateNew(url);
        std::string sid = MediaSessionManager::Instance().AddSession(media_session, url);
        media_session->SetId(std::stoi(sid));
    }

    std::string sessionId = std::to_string(media_session->GetId());
    LOG_INFO("sessionId:" + sessionId);

    std::shared_ptr<RtpTrack> track_ptr;
    if (!BindTrackByControl(controlTdx, media_session, track_ptr)) 
    {
        LOG_ERROR("BindTrackByControl failed, control=", controlTdx);
        return;
    }

    if (!rtp_connection_)
        rtp_connection_ = std::make_shared<RtpConnection>(conn_);

    std::shared_ptr<char> response(new char[10240], std::default_delete<char[]>());
    int size = 0;

    MediaChannelId channel_id = rtsp_request_->GetSessionId();

    if (rtsp_request_->GetTransport() == RTP_OVER_TCP)
    {
        uint16_t rtp_ch  = rtsp_request_->GetRtpChannel();
        uint16_t rtcp_ch = rtsp_request_->GetRtcpChannel();

        if (rtp_ch > 255 || rtcp_ch > 255 || rtp_ch == rtcp_ch) 
        {
            LOG_ERROR("Invalid interleaved channels rtp=", rtp_ch, " rtcp=", rtcp_ch);
            return;
        }

        if (!rtp_connection_->SetupRtpOverTcp(channel_id, rtp_ch, rtcp_ch)) 
        {
            LOG_ERROR("SetupRtpOverTcp failed");
            return;
        }

        interleaved_.bind((uint8_t)rtp_ch,  track_ptr, false);
        interleaved_.bind((uint8_t)rtcp_ch, track_ptr, true);

        size = rtsp_request_->BuildSetupRes(response, 10240,
                                            rtp_ch, rtcp_ch,
                                            channel_id, sessionId);
    }
    else if (rtsp_request_->GetTransport() == RTP_OVER_UDP)
    {
        LOG_ERROR("RTP_OVER_UDP not implemented yet");
        return;
    }
    else
    {
        LOG_ERROR("Unsupported transport mode for SETUP");
        return;
    }

    if (size <= 0) {
        LOG_ERROR("BuildSetupRes failed, size=", size);
        return;
    }

    media_session->AddClient(channel_id, rtp_connection_);
    conn_->Send(response.get(), (size_t)size);
}
void RtspSession::HandleCmdRecord()
{
    //track轨道中是否存在
    std::string url = rtsp_request_->GetRtspUSuffix();
    LOG_INFO("RECORD request for url=" + url);

    auto media_session = MediaSessionManager::Instance().GetSessionBySuffix(url);
    if (!media_session)
    {
        LOG_INFO("No existing MediaSession found for url=" + url + ", creating new one...");
        return;
    }
   
    if (media_session->tracks_.empty()) 
    {
        LOG_DEBUG("No tracks in session, cannot RECORD");
        return;
    }

    // 遍历 track 初始化 RTP
    for (auto &track : media_session->tracks_) 
    {
        if (!track->_inited)
        {
            //track->_ssrc = GenerateSSRC();
            track->_seq = 0;
            track->_time_stamp = 0;
            track->_inited = true;
        }
    }
    
    std::shared_ptr<char> res(new char[2048], std::default_delete<char[]>());
    int size = rtsp_request_->BuildRecordRes(res, 2048,std::to_string(session_id_));
    this->SendRaw(res.get(), size);
}
void RtspSession::HandleCmdPlay()
{

}
void RtspSession::HandleCmdPause()
{
}
void RtspSession::HandleCmdTeardown()
{
}
}