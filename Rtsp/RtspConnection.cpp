#include "RtspConnection.h"

RtspConnection::RtspConnection(std::shared_ptr<RtspServer> rtsp_server, TaskScheduler *task_scheduler, SOCKET sockfd)
    : TcpConnection(task_scheduler, sockfd)
    , rtsp_server_(rtsp_server)
    ,task_scheduler_(task_scheduler)
    ,rtsp_request_(std::make_unique<RtspRequest>())
    ,read_buffer_(std::make_unique<RtspResponse>())
    ,rtsp_(std::make_shared<Rtsp>())
{
    // Initialize the connection
    LOG_INFO("RtspConnection created with sockfd: " + std::to_string(sockfd));
    active_ = true;
}

RtspConnection::~RtspConnection()
{
}

/*    回复服务器支持的操作 SETUP、PLAY、PAUSE、TEARDOWN
    * @brief 处理接收到的消息
    * @param buffer 消息缓冲区
    */

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
    switch(method)
    {
        case RtspRequest::Method::OPTIONS:
            LOG_INFO("Handling OPTIONS request");
            HandleCmdOptions();
            break;
        case RtspRequest::Method::DESCRIBE:
            LOG_INFO("Handling DESCRIBE request");
            HandleCmdDescribe();
            break;
        case RtspRequest::Method::SETUP:
            HandleCmdSetup();
            break;
        case RtspRequest::Method::PLAY:
            HandleCmdPlay();
            break;
        case RtspRequest::Method::PAUSE:
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

bool RtspConnection::HandleRtspResponse(BufferReader &buffer)
{
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
    int size = 0;

    //创建RTPConnection对象
    if (rtp_connection_ == nullptr) {
		rtp_connection_.reset(new RtpConnection(shared_from_this()));
	}

	std::shared_ptr<char> res(new char[4096], std::default_delete<char[]>());
	MediaSession::Ptr media_session = nullptr;

    //判断suffix, 后面媒体会话
    if (rtsp_) 
    {
            media_session = rtsp_->LookMediaSession(rtsp_request_->GetRtspUSuffix());
            if (!media_session) 
            {
                LOG_ERROR("Media session not found for suffix: " + rtsp_request_->GetRtspUSuffix());
                
                return;
            }
    }


    //客户端添加到会话,设置RTP的参数
    if (media_session) 
    {
        session_id_ = media_session->GetId();
        //media_session->AddClient(this->GetSocket(),session_id_); // 0 is a placeholder for channel_id

        for(int i = 0; i < MAX_MEDIA_CHANNEL; ++i) 
        {
            rtp_connection_->SetClockrate((MediaChannelId)i, media_session->GetMediaChannelClockRate((MediaChannelId)i));
            rtp_connection_->SetPlayLoadType((MediaChannelId)i, media_session->GetMediaChannelPayloadType((MediaChannelId)i));
        }

    }
    else 
    {
        LOG_ERROR("Media session is null");
        return;
    }




}

void RtspConnection::HandleCmdSetup()
{
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

bool RtspConnection::onClose()
{
    return false;
}
