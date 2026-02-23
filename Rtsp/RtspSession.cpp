#include "RtspSession.h"
namespace rtsp 
{
bool RtspSession::OnRead(TcpConnection::Ptr conn, BufferReader& buffer)
{
    size_t readable = buffer.ReadableBytes();

    LOG_INFO("[RTSP] OnRead fd=" 
             + std::to_string(conn->GetSocket()) 
             + " readable=" 
             + std::to_string(readable));

    if (readable == 0)
        return false;

    size_t n = buffer.ReadableBytes();
    size_t dump = std::min<size_t>(n, 200);
    std::string s(buffer.Peek(), dump);
    LOG_INFO("[RTSP] RAW:\n" + s);
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

void RtspSession::Dispatch(RtspRequest::RtspRequestInfo &req)
{
    if (req.method == "OPTIONS")  { HandleCmdOptions();  return; }
    if (req.method == "DESCRIBE") { HandleCmdDescribe(); return; }
    if (req.method == "SETUP")    { HandleCmdSetup();    return; }
    if (req.method == "PLAY")     { HandleCmdPlay();     return; }
    if (req.method == "PAUSE")    { HandleCmdPause();    return; }
    if (req.method == "TEARDOWN") { HandleCmdTeardown(); return; }
    if (req.method == "RECORD")   { HandleCmdRecord();   return; }

    // SendError(req.cseq, 501, "Not Implemented");
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