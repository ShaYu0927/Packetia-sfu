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

bool RtspConnection::onRead(BufferReader &buffer)
{
    return false;
}

bool RtspConnection::onClose()
{
    return false;
}
