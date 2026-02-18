#include "RtspSession.h"
namespace rtsp 
{
bool RtspSession::OnRead(TcpConnection::Ptr conn, BufferReader& buffer)
{
    LOG_INFO("RtspSession::OnRead called, sockfd: " + std::to_string(conn->GetSocket()));

    if (buffer.ReadableBytes() <= 0)
    {
        LOG_ERROR("Buffer is empty or error occurred");
        return false;
    }
    return true;
}

void RtspSession::Start()
{
    LOG_INFO("RtspSession started for sockfd: " + std::to_string(conn_->GetSocket()));
}

}