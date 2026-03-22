#include "RtspConnection.h"
#include "logger.h"


RtspConnection::RtspConnection(std::shared_ptr<RtspServer> rtsp_server, TaskScheduler *task_scheduler, SOCKET sockfd)
    : TcpConnection(task_scheduler, sockfd)
    , rtsp_server_(rtsp_server)
    ,task_scheduler_(task_scheduler)
{
    LOG_INFO("RtspConnection created with sockfd: " + std::to_string(sockfd));
    active_ = true;
}

std::shared_ptr<RtspConnection> RtspConnection::Create(std::shared_ptr<RtspServer> rtsp_server, TaskScheduler *task_scheduler, SOCKET sockfd)
{
    auto conn = std::shared_ptr<RtspConnection>(new RtspConnection(rtsp_server, task_scheduler, sockfd));

    conn->InitCallbacks();

    return conn;
}

RtspConnection::~RtspConnection()
{
}

void RtspConnection::InitCallbacks()
{

    
}

void RtspConnection::OnMessage(BufferReader *buffer)
{
}


void RtspConnection::SendRtspMessage(std::shared_ptr<char> data, uint32_t size)
{
    LOG_DEBUG("RTSP message content (" + std::to_string(size) + " bytes):");
    LOG_DEBUG("\n" + std::string(data.get(), size));
    LOG_DEBUG("End of RTSP message content");
	return;
}

bool RtspConnection::onRead(BufferReader &buffer)
{


    return true;
}

