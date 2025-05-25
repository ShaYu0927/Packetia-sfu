#include "RtspConnection.h"

RtspConnection::RtspConnection(std::shared_ptr<RtspServer> rtsp_server, TaskScheduler *task_scheduler, SOCKET sockfd)
    : TcpConnection(task_scheduler, sockfd)
    , rtsp_server_(rtsp_server)
    ,task_scheduler_(task_scheduler)
    ,rtsp_request_(std::make_unique<RtspRequest>())
    ,read_buffer_(std::make_unique<RtspResponse>())
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
    // RtspRequest::Method method = rtsp_request_->GetMethod();
    // switch(method)
    // {
    //     case RtspRequest::Method::OPTIONS:
    //         LOG_INFO("Handling OPTIONS request");
    //         HandleCmdOptions();
    //         break;
    //     case RtspRequest::Method::DESCRIBE:
    //         HandleCmdDescribe();
    //         break;
    //     case RtspRequest::Method::SETUP:
    //         HandleCmdSetup();
    //         break;
    //     case RtspRequest::Method::PLAY:
    //         HandleCmdPlay();
    //         break;
    //     case RtspRequest::Method::PAUSE:
    //         HandleCmdPause();
    //         break;
    //     case RtspRequest::Method::TEARDOWN:
    //         HandleCmdTeardown();
    //         break;
    //     default:
    //         LOG_ERROR("Unsupported RTSP method: " + rtsp_request_->GetMethodString());
    //         return false;
    // }
    
    return true;
}

bool RtspConnection::HandleRtspResponse(BufferReader &buffer)
{
    return false;
}

void RtspConnection::HandleCmdOptions()
{
}

void RtspConnection::HandleCmdDescribe()
{
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
