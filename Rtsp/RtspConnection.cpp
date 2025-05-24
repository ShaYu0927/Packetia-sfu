#include "RtspConnection.h"

RtspConnection::RtspConnection(std::shared_ptr<RtspServer> rtsp_server, TaskScheduler *task_scheduler, SOCKET sockfd)
    : TcpConnection(task_scheduler, sockfd)
    , rtsp_server_(rtsp_server)
    ,task_scheduler_(task_scheduler)
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

    
    return true;
}

bool RtspConnection::HandleRtspResponse(BufferReader &buffer)
{
    return false;
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
