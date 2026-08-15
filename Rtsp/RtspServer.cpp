#include "RtspServer.h"
#include "RtspSession.h"


RtspServer::RtspServer(EventLoop* event_loop)
    : TcpServer(event_loop)
{
    LOG_INFO("RtspServer created with event loop: " + std::to_string(reinterpret_cast<uintptr_t>(event_loop)));

}

RtspServer::~RtspServer()
{
}

TcpConnection::Ptr RtspServer:: OnConnect(SOCKET sockfd)
{
    LOG_INFO("New RTSP connection established with sockfd: " + std::to_string(sockfd));

    auto conn = RtspConnection::Create(shared_from_this(),
                                       event_loop_->GetTaskScheduler().get(),
                                       sockfd);

    auto rtsp_conn = std::dynamic_pointer_cast<RtspConnection>(conn);
    if (!rtsp_conn)
    {
        LOG_ERROR("cast to RtspConnection failed, sockfd=" + std::to_string(sockfd));
        return conn;
    }

    auto session = std::make_shared<rtsp::RtspSession>(rtsp_conn);
    sessions_[sockfd] = session;
    std::weak_ptr<rtsp::RtspSession> weak_session = session;
    conn->SetReadCallback(
        [weak_session](TcpConnection::Ptr conn, BufferReader& buffer) -> bool
        {
            auto session = weak_session.lock();
            if (!session)
            {
                LOG_ERROR("RtspSession expired, fd=" + std::to_string(conn->GetSocket()));
                return false;
            }
            return session->OnRead(conn, buffer);
        });

    conn->Start();
    return conn;
}

void RtspServer::RemoveConnection(SOCKET sockfd)
{
    auto it = sessions_.find(sockfd);
    if (it != sessions_.end())
    {
        it->second->OnClosed(0);
        sessions_.erase(it);
    }
    TcpServer::RemoveConnection(sockfd);
}
