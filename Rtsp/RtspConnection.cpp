#include "RtspConnection.h"
#include "RtpTypes.h"
#include "logger.h"


RtspConnection::RtspConnection(std::shared_ptr<RtspServer> rtsp_server, TaskScheduler *task_scheduler, SOCKET sockfd)
    : TcpConnection(task_scheduler, sockfd)
    , rtsp_server_(rtsp_server)
    ,task_scheduler_(task_scheduler)
    ,rtsp_request_(std::make_unique<RtspRequest>())
    ,sdp_(std::make_unique<Sdp>())
    ,read_buffer_(std::make_unique<RtspResponse>())
    ,rtsp_(std::make_shared<Rtsp>())
    ,packet_pool_(std::make_unique<PacketPool>(2048, 1000))
{
    // Initialize the connection
    LOG_INFO("RtspConnection created with sockfd: " + std::to_string(sockfd));
    active_ = true;

    interleaved_.SetPacketPool(packet_pool_.get());

    auto rtp_handler_ = std::make_shared<RtpJobHandler>(packet_pool_.get());
   

    media_pool_ = WorkerService::get_pool("media");
    if (!media_pool_) 
    {
        WorkerService::create_pool(
            "media", 4, rtp_handler_, 4096,
            ShardedWorkerPool::DropPolicy::DropHead);
        media_pool_ = WorkerService::get_pool("media");

    }


    


}

std::shared_ptr<RtspConnection> RtspConnection::Create(std::shared_ptr<RtspServer> rtsp_server, TaskScheduler *task_scheduler, SOCKET sockfd)
{
    auto conn = std::shared_ptr<RtspConnection>(new RtspConnection(rtsp_server, task_scheduler, sockfd));

    conn->InitCallbacks();
    conn->Start();

    return conn;
}

RtspConnection::~RtspConnection()
{
}

void RtspConnection::InitCallbacks()
{
    std::weak_ptr<RtspConnection> weak_self =
    std::static_pointer_cast<RtspConnection>(TcpConnection::shared_from_this());


    this->SetReadCallback([weak_self](std::shared_ptr<TcpConnection>, BufferReader& buffer) -> bool {
        auto self = weak_self.lock();
        if (!self) return false;          // 对象已释放，直接忽略
        return self->onRead(buffer);
    });

    this->SetCloseCallback([weak_self](std::shared_ptr<TcpConnection>) -> bool {
        auto self = weak_self.lock();
        if (!self) return false;
        return self->onClose();
    });
}

/*    回复服务器支持的操作 SETUP、PLAY、PAUSE、TEARDOWN
    * @brief 处理接收到的消息
    * @param buffer 消息缓冲区
    */

void RtspConnection::OnMessage(BufferReader *buffer)
{
}

bool RtspConnection::onClose()
{
    if(session_id_ != 0)
    {
        LOG_INFO("Closing RtspConnection, session_id: " + std::to_string(session_id_));
        auto rtsp = rtsp_.get();
        if (rtsp) 
        {
            auto MediaSession = rtsp_->LookMediaSession(session_id_);
            if(MediaSession)
            {
                LOG_INFO("Removing MediaSession with id: " + std::to_string(session_id_));
                MediaSession->RemoveClient(this->GetSocket());
            }
            else
            {
                LOG_ERROR("MediaSession not found for session_id: " + std::to_string(session_id_));
            }
        }
        session_id_ = 0;
    }

    for(int n = 0; n < MAX_MEDIA_CHANNEL; ++n)
    {
       if(rtcp_channels_[n] && rtcp_channels_[n]->IsNoneEvent())
       {
           LOG_INFO("Closing RTCP channel for MediaChannelId: " + std::to_string(n));
           task_scheduler_->RemoveChannel(rtcp_channels_[n]);
       }
    }
    return true;
}

int RtspConnection::ParseStreamId(const std::string& control)
{
    // streamid=NUMBER
    static const std::string kKey = "streamid=";

    auto pos = control.find(kKey);
    if (pos == std::string::npos) 
    {
        LOG_ERROR("ParseStreamId: no 'streamid=' in control=", control);
        return -1;
    }

    std::string num = control.substr(pos + kKey.size());
    if (num.empty()) 
    {
        LOG_ERROR("ParseStreamId: empty streamid in control=", control);
        return -1;
    }

    try {
        int idx = std::stoi(num);
        if (idx < 0) {
            LOG_ERROR("ParseStreamId: negative streamid=", idx);
            return -1;
        }
        return idx;
    }
    catch (const std::exception& e) {
        LOG_ERROR("ParseStreamId: invalid streamid=", num, " err=", e.what());
        return -1;
    }
}


bool RtspConnection::HandleRtspRequest(BufferReader &buffer)
{
    if (buffer.ReadableBytes() <= 0)
    {
        LOG_ERROR("Buffer is empty or invalid");
        return false;
    }
    std::string str(buffer.Peek(), buffer.ReadableBytes());
	if (str.find("rtsp") != std::string::npos || str.find("RTSP") != std::string::npos)
	{
		LOG_INFO("Received RTSP request: " + str);
	}
    LOG_INFO("Parsing RTSP request from buffer, size: " + std::to_string(buffer.ReadableBytes()));
    if (!rtsp_request_->ParseRequest(&buffer)) 
    {
        LOG_ERROR("Failed to parse RTSP request");
        return false;
    }
    RtspRequest::Method method = rtsp_request_->GetMethod();
    LOG_INFO("Parsed RTSP method: " + rtsp_request_->GetMethodString());
    switch(method)
    {
        case RtspRequest::Method::OPTIONS:
            LOG_INFO("Handling OPTIONS request");
            HandleCmdOptions();
            rtsp_request_->Reset(); // 重置请求状态
            break;
        case RtspRequest::Method::DESCRIBE:
            LOG_INFO("Handling DESCRIBE request");
            HandleCmdDescribe();
            break;
        case RtspRequest::Method::SETUP:
            LOG_INFO("Handing SETUP request");
            HandleCmdSetup();
            break;
        case RtspRequest::Method::PLAY:
            LOG_INFO("Handing Play request");
            HandleCmdPlay();
            break;
        case RtspRequest::Method::PAUSE:
            LOG_INFO("Hading Pause request");
            HandleCmdPause();
            break;
        case RtspRequest::Method::TEARDOWN:
            HandleCmdTeardown();
            break;
        case RtspRequest::Method::ANNOUNCE:
            LOG_INFO("Handling ANNOUNCE request");
            HandleCmdANNOUNCE();
            break;
        case RtspRequest::Method::RECORD:
            LOG_INFO("Handling RECORD request");
            HandleCmdRecord();
            break;
        default:
            LOG_ERROR("Unsupported RTSP method: " + rtsp_request_->GetMethodString());
            return false;
    }
    
    return true;
}

/**
 * @brief 处理 RTSP 响应 ,适用于推流、拉流
 * @param buffer 响应缓冲区
 * @return 是否处理成功
 */

bool RtspConnection::HandleRtspResponse(BufferReader &buffer)
{
    LOG_INFO("RtspConnection::HandleRtspResponse called, sockfd: " + std::to_string(this->GetSocket()));

    return false;
}

void RtspConnection::HandleCmdOptions()
{
    std::shared_ptr<char> res(new char[2048], std::default_delete<char[]>());
    int size = rtsp_request_->BuildOptionsRes(res, 1024);
    LOG_INFO("Handling OPTIONS request, response size: " + std::to_string(size));
    this->SendRtspMessage(res, size);	
}

//客户端播放流,服务器协商拟定
void RtspConnection::HandleCmdDescribe()
{
    LOG_INFO("Handling DESCRIBE request");

    if (!rtsp_) {
        LOG_ERROR("RTSP context is null");
        return;
    }

    // 查找媒体会话
    auto suffix = rtsp_request_->GetRtspUSuffix();
    auto media_session = rtsp_->LookMediaSession(suffix);
    if (!media_session) {
        LOG_ERROR("Media session not found for suffix: create suffix" + suffix);
        std::shared_ptr<char> errorRes(new char[256], std::default_delete<char[]>());
        media_session = MediaSession::CreateNew(suffix);
        rtsp_->AddMediaSession(media_session);
    }

    // 初始化 RTP connection
    if (!rtp_connection_) 
    {
        rtp_connection_ = std::make_shared<RtpConnection>(shared_from_this());
    }

    // 设置 RTP 参数
    session_id_ = media_session->GetId();
    LOG_INFO("Setting session_id: " + std::to_string(session_id_));
    for (int i = 0; i < MAX_MEDIA_CHANNEL; ++i) 
    {
        rtp_connection_->SetClockrate((MediaChannelId)i, media_session->GetMediaChannelClockRate((MediaChannelId)i));
        rtp_connection_->SetPlayLoadType((MediaChannelId)i, media_session->GetMediaChannelPayloadType((MediaChannelId)i));
    }

    // 构建 SDP
    std::shared_ptr<char> res(new char[4096], std::default_delete<char[]>());
    int size = 0;
    std::string sdp_message = sdp_->GetSdpMessage(rtsp_->rtsp_url_info_.ip, media_session->GetRtspSuffix());
    if(sdp_message == "")
    {
        size = rtsp_request_->BuildNotFoundRes(res,4096);
    }
    else
    {
        size = rtsp_request_->BuildDescribeRes(res, 4096, sdp_message);
    }
   
    this->SendRtspMessage(res, size);
}

void RtspConnection::HandleCmdANNOUNCE()
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
    this->SendRtspMessage(res, MessageSize);
}

/*
    处理客户端 SETUP 请求，根据传输模式（TCP/UDP/组播）为每个 track 建立对应的 RTP 通道，并构造符合 RTSP 标准的响应报文
*/

void RtspConnection::HandleCmdSetup()
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

    int trackIdx = -1;
    std::shared_ptr<RtpTrack> track_ptr;

   
    bool found = false;
    for (auto& m : rtsp_request_->sdp_->media_list_) 
    {
        if (m.control == controlTdx) 
        {
            found = true;
            trackIdx = ParseStreamId(m.control);
            TrackType type = (m.media_type == "video") ? TrackType::TrackVideo : TrackType::TrackAudio;
            track_ptr = createTrack(type, m.codec_name, m.payload_type, m.clock_rate, trackIdx );
            media_session->AddTrack(type, m.codec_name, m.control /*control*/,
                        m.payload_type, m.clock_rate);
            MediaSessionManager::Instance().AddTrackChannel(trackIdx, track_ptr);

            /* bind the sending entity to the session */
            media_session->BindRtpTrack(trackIdx, track_ptr);
            break;
        }
    }
    if (!found || trackIdx < 0 || !track_ptr) 
    {
        LOG_ERROR("SETUP: track not found, control=", controlTdx);
        // 回复 404/461
        return;
    }

    if (!rtp_connection_) rtp_connection_ = std::make_shared<RtpConnection>(shared_from_this());

    std::shared_ptr<char> res(new char[10240], std::default_delete<char[]>());
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
            return;
        }

        // 关键：channel -> track 绑定（用于接收端切包分发）
        interleaved_.bind((uint8_t)rtp_ch,  track_ptr, false);
        interleaved_.bind((uint8_t)rtcp_ch, track_ptr, true);
        


        size = rtsp_request_->BuildSetupRes(res, 4096, rtp_ch, rtcp_ch, channel_id, sessionId);
    }
    else if (rtsp_request_->GetTransport() == RTP_OVER_UDP) 
    {
        // TODO: 这里建议按 client_port/server_port 语义重做
       
    }
    else 
    {
        LOG_ERROR("Unsupported transport mode for SETUP");
        return;
    }

    SendRtspMessage(res, size);
    media_session->AddClient(channel_id, rtp_connection_);
}

void RtspConnection::HandleCmdRecord()
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

    // 启动 RTP OVER TCP 推送线程
    

    LOG_INFO("Init RECORD for session: " + media_session->GetId());
    std::shared_ptr<char> res(new char[2048], std::default_delete<char[]>());
    int size = rtsp_request_->BuildRecordRes(res, 2048,std::to_string(session_id_));
    this->SendRtspMessage(res, size);
}




/**
 *  确认 Session ID 是否存在且有效。需要保证多个track都注册不同的session ID。
    确认客户端是否已经做过 SETUP。
    确认请求的 URL（stream 或 track）是否合法。
*/

void RtspConnection::HandleCmdPlay()
{
    //需要校验前面的SETUP逻辑,在前面初始化了session层
    LOG_INFO("Handling PLAY request");
    if (!rtsp_) 
    {
        LOG_ERROR("RTSP context is null");
        return;
    }

    auto suffix = rtsp_request_->GetRtspUSuffix(); //获取当前流的suffix
    auto media_session = rtsp_->LookMediaSession(suffix); //查找对应的session
    if(!media_session)
    {
        LOG_ERROR("Media session not found for suffix: " + suffix);
        std::shared_ptr<char> errorRes(new char[256], std::default_delete<char[]>());
        rtsp_request_->BuildNotFoundRes(errorRes, 256);
        this->SendRtspMessage(errorRes, 256);
        return;
    }

    //获取RTP的连接对象
    if (!rtp_connection_) 
    {
        rtp_connection_ = std::make_shared<RtpConnection>(shared_from_this());
    }

    RTPTransportMode mode = rtp_connection_->GetTransport();

    
    //开始推送流,但目前没有做推流，而是模拟拉流
     if (mode == RTPTransportMode::RTP_OVER_TCP) 
     {
        // 设置 TCP 推流
        LOG_INFO("Starting RTP stream over TCP");
        //rtp_connection_->StartTcpStream();
    } else if (mode == RTPTransportMode::RTP_OVER_UDP) 
    {
        // 设置 UDP 推流
        LOG_INFO("Starting RTP stream over UDP");
        //rtp_connection_->StartUdpStream();
    } 
    else if (mode == RTPTransportMode::RTP_OVER_MULTICAST) 
    {
        // 设置 Multicast 推流
        LOG_INFO("Starting RTP stream over Multicast");
        //rtp_connection_->StartMulticastStream();
    }


}

void RtspConnection::HandleCmdPause()
{
}

void RtspConnection::HandleCmdTeardown()
{
}

void RtspConnection::SendRtspMessage(std::shared_ptr<char> data, uint32_t size)
{
#if RTSP_DEBUG
	cout << buf.get() << endl;
#endif
    LOG_DEBUG("RTSP message content (" + std::to_string(size) + " bytes):");
    LOG_DEBUG("\n" + std::string(data.get(), size));
    LOG_DEBUG("End of RTSP message content");
	this->Send(data, size);
	return;
}

/*
    interleaved packets: Two interleaved packets are read out at once
    Unpacking: An interleaved package needs to be read twice to be fully read
    Interleaved: The $package and RTSP text appear simultaneously
*/

/* $ <channel:1B> <length:2B big-endian> <payload:length bytes> */
int RtspConnection::RtspConn_ConsumeInterleaved(BufferReader &buffer)
{
    if (buffer.ReadableBytes() < 4) return false;

    const uint8_t* p = reinterpret_cast<const uint8_t*>(buffer.Peek());
    if (p[0] != '$') return false;

    uint8_t  ch  = p[1];
    uint16_t len = (static_cast<uint16_t>(p[2]) << 8) | p[3];

    if(buffer.ReadableBytes() < len + 4)
    {
        return false;
    }
    LOG_INFO("Start handle data:", buffer.ReadableBytes());

    int rc = interleaved_.onInterleaved(ch, p + 4, len);
    LOG_INFO("End handle data result:", rc);
    if(rc < 0)
    {
        buffer.Retrieve(len + 4);
        LOG_INFO("RC handle falied");
        return false;
    }
    buffer.Retrieve(len + 4); /* consumer buffer */
    return true;
}

bool RtspConnection::onRead(BufferReader &buffer)
{
    int size = buffer.ReadableBytes();
    if (size <= 0) return false;

    if (mode_ == RTSP_SERVER)
    {
        /* 数据面：循环消费所有完整的 interleaved 包 */
        while (RtspConn_ConsumeInterleaved(buffer)) 
        {
            continue;
        }

        if (buffer.ReadableBytes() == 0) 
        {
            return true;
        }

        const uint8_t* p = reinterpret_cast<const uint8_t*>(buffer.Peek());
        if (p[0] == '$') return true;

        
        size_t before = buffer.ReadableBytes();
        bool ok = HandleRtspRequest(buffer);
        if (!ok) return false;

        size_t after = buffer.ReadableBytes();
        if (after == before) 
        {
           /* part packet*/
            return true;
        }
    }
    else if (mode_ == RTSP_PUSHER)
    {
        /* 推流端如果也可能收到 interleaved（视你的设计），同理也可先 consume */
        if (!HandleRtspResponse(buffer)) {
            return false;
        }
    }

    return true;
}


std::shared_ptr<RtpTrack> createTrack(TrackType type, const std::string &codec_name, int payload_type, uint32_t clock_rate, int track_index)
{
     if (type == TrackType::TrackVideo) {
        return std::make_shared<RtpVideoTracker>(
            type, codec_name, payload_type, 0, clock_rate, track_index);
    }

    return std::make_shared<RtpAudioTracker>(
        type, codec_name, payload_type, 0, clock_rate, track_index);
}
