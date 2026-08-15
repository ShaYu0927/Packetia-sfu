#include "SipServer.h"

#include "logger.h"

SipServer::SipServer(EventLoop* event_loop)
    : TcpServer(event_loop)
{
    LOG_INFO("SipServer created with event loop: ", reinterpret_cast<uintptr_t>(event_loop));

}

SipServer::~SipServer() = default;

TcpConnection::Ptr SipServer::OnConnect(SOCKET sockfd)
{
    LOG_INFO("New SIP connection established, sockfd=", sockfd);
    auto conn = std::make_shared<TcpConnection>(event_loop_->GetTaskScheduler().get(), sockfd);
    auto session = std::make_shared<sip::SipSession>(conn);
    sessions_[sockfd] = session;

    std::weak_ptr<sip::SipSession> weak_session = session;
    conn->SetReadCallback([weak_session](TcpConnection::Ptr current, BufferReader& buffer) {
        auto active = weak_session.lock();
        return active && active->OnRead(std::move(current), buffer);
    });
    conn->Start();
    return conn;
}

void SipServer::RemoveConnection(SOCKET sockfd)
{
    auto it = sessions_.find(sockfd);
    if (it != sessions_.end())
    {
        it->second->OnClosed(0);
        sessions_.erase(it);
    }
    TcpServer::RemoveConnection(sockfd);
}
