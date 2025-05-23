#include "RtspConnection.h"

RtspConnection::RtspConnection(std::shared_ptr<Rtsp> rtsp_server, TaskScheduler *task_scheduler, SOCKET sockfd)
    : TcpConnection(task_scheduler, sockfd)
    , rtsp_server_(rtsp_server)
{
    // Initialize the connection
    active_ = true;
    this->SetReadCallback([this](std::shared_ptr<TcpConnection> conn, BufferReader& buffer) {
		return this->onRead(buffer);
	});

    this->SetCloseCallback([this](std::shared_ptr<TcpConnection> conn) {
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

bool RtspConnection::HandleRtspRequest(BufferReader &buffer)
{
    std::string str(buffer.Peek(), buffer.ReadableBytes());
	if (str.find("rtsp") != std::string::npos || str.find("RTSP") != std::string::npos)
	{
		std::cout << str << std::endl;
	}

    
    return false;
}

bool RtspConnection::HandleRtspResponse(BufferReader &buffer)
{
    return false;
}

bool RtspConnection::onRead(BufferReader &buffer)
{
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
