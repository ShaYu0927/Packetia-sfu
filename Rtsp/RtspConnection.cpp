#include "RtspConnection.h"

RtspConnection::RtspConnection(std::shared_ptr<RtspServer> rtsp_server, TaskScheduler *task_scheduler, SOCKET sockfd)
    : TcpConnection(task_scheduler, sockfd)
    , rtsp_server_(rtsp_server)
    ,task_scheduler_(task_scheduler)
    ,rtsp_request_(std::make_unique<RtspRequest>())
    ,sdp_(std::make_unique<Sdp>())
    ,read_buffer_(std::make_unique<RtspResponse>())
    ,rtsp_(std::make_shared<Rtsp>())
{
    // Initialize the connection
    LOG_INFO("RtspConnection created with sockfd: " + std::to_string(sockfd));
    active_ = true;

    //初始化 RTP 与 Rtcp 通道
    this->SetReadCallback([this](std::shared_ptr<TcpConnection> conn, BufferReader &buffer) -> bool {
        return this->onRead(buffer);
    });

    this->SetCloseCallback([this](std::shared_ptr<TcpConnection> conn) -> bool{
        return this->onClose();
    }); 

}

RtspConnection::~RtspConnection()
{
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

bool RtspConnection::HandleRtspRequest(BufferReader &buffer)
{
    LOG_INFO("RtspConnection::HandleRtspRequest called, sockfd: " + std::to_string(this->GetSocket()));
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

/*
    处理客户端 SETUP 请求，根据传输模式（TCP/UDP/组播）为每个 track 建立对应的 RTP 通道，并构造符合 RTSP 标准的响应报文
*/
void RtspConnection::HandleCmdSetup() 
{
    LOG_INFO("Handling SETUP request");
    if (!rtsp_) 
    {
        LOG_ERROR("RTSP context is null");
        return;
    }

    std::shared_ptr<char> res(new char[10240], std::default_delete<char[]>());
    int size = 0;
    MediaChannelId channel_id = rtsp_request_->GetSessionId();
    auto rtsp = rtsp_; // 避免 move 导致后续失效

    auto media_session = rtsp->LookMediaSession(rtsp_request_->GetRtspUSuffix());
    if (!media_session) 
    {
        LOG_INFO("Media session is created" + rtsp_request_->GetRtspUSuffix());
        media_session = MediaSession::CreateNew(rtsp_request_->GetRtspUSuffix()); 
        rtsp->AddMediaSession(media_session);
    }

    if (!rtp_connection_) 
    {
        rtp_connection_ = std::make_shared<RtpConnection>(shared_from_this());
    }

    if(media_session->isMulticast())  //如果组包传输
    {
        std::string multicast_ip = media_session->GetMulticastIp();
        if(rtsp_request_->GetTransport() == RTP_OVER_MULTICAST)
        {
            uint16_t port = media_session->GetMulticastPort(channel_id);
            auto session_id = rtp_connection_->GetRtpSessionId();
            LOG_INFO("Setting up multicast RTP connection for channel: " + std::to_string(channel_id));
            if (!rtp_connection_->SetupRtpOverMulticast(channel_id, multicast_ip, media_session->GetMulticastPort(channel_id))) 
            {
                LOG_ERROR("Failed to setup RTP over multicast for channel: " + std::to_string(channel_id));
                size = rtsp_request_->BuildNotFoundRes(res, 4096);
                this->SendRtspMessage(res, size);
                return;
            }
            size = rtsp_request_->BuildSetupMulticastRes(res, 4096, multicast_ip.c_str(), port, session_id);
        }
        else
        {
            LOG_ERROR("Invalid transport type for multicast setup");
            int size = rtsp_request_->BuildNotFoundRes(res, 4096);
            this->SendRtspMessage(res, size);
            LOG_ERROR("Invalid transport type for multicast setup");
            return;
        }

    }
    else
    {
        //判读是否是 TCP 传输 或者 UDP 传输
        if(rtsp_request_->GetTransport() == RTP_OVER_TCP)
        {
            LOG_INFO("RTP OVER TCP");
            uint16_t rtp_channel = rtsp_request_->GetRtpChannel();
            uint16_t rtcp_channel = rtsp_request_->GetRtcpChannel();
            if (!rtp_connection_->SetupRtpOverTcp(channel_id, rtp_channel, rtcp_channel)) 
            {
                LOG_ERROR("Failed to setup RTP over TCP for channel: " + std::to_string(channel_id));
                size = rtsp_request_->BuildNotFoundRes(res, 4096);
                this->SendRtspMessage(res, size);
                return;
            }
            size = rtsp_request_->BuildSetupRes(res, 4096, rtp_channel, rtcp_channel, channel_id);
        }
        else if(rtsp_request_->GetTransport() == RTP_OVER_UDP)
        {
            LOG_INFO("RTP OVER UDP");
            uint16_t rtp_port = rtsp_request_->GetRtpPort();
            uint16_t rtcp_port = rtsp_request_->GetRtcpPort();
            if (!rtp_connection_->SetupRtpOverUdp(channel_id, rtp_port, rtcp_port)) 
            {
                LOG_ERROR("Failed to setup RTP over UDP for channel: " + std::to_string(channel_id));
                size = rtsp_request_->BuildNotFoundRes(res, 4096);
                this->SendRtspMessage(res, size);
                return;
            }
            size = rtsp_request_->BuildSetupRes(res, 4096, rtp_port, rtcp_port, channel_id);
        }
        else
        {
            LOG_ERROR("Unsupported transport mode for SETUP");
            return;
        }
    }
    this->SendRtspMessage(res, size);
    media_session->AddClient(channel_id,rtp_connection_);
}


void RtspConnection::HandleCmdPlay()
{
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

bool RtspConnection::onRead(BufferReader &buffer)
{
    LOG_INFO("RtspConnection::onRead called, sockfd: " + std::to_string(this->GetSocket()));
    int size = buffer.ReadableBytes();
    if (size <= 0)
    {
        return false;
    }

    if (mode_ == RTSP_SERVER) {

		if (!HandleRtspRequest(buffer))
        {
			return false; 
		}
	}
	else if (mode_ == RTSP_PUSHER) {
		if (!HandleRtspResponse(buffer)) 
        {           
			return false;
		}
	}

    return true;
}

