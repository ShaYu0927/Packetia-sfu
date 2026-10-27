#include "RtspServer.h"
#include "RtspSession.h"


RtspServer::RtspServer(EventLoop* event_loop, service::auth::RtspAuthOptions auth)
    : TcpServer(event_loop), auth_options_(std::move(auth))
{
    auth_options_.Validate();
    if (auth_options_.enabled)
        credentials_ = std::make_shared<service::auth::StaticRtspCredentialProvider>(auth_options_);
    LOG_INFO("RtspServer created with event loop: " + std::to_string(reinterpret_cast<uintptr_t>(event_loop)));

}

RtspServer::~RtspServer()
{
    Stop();
}

TcpConnection::Ptr RtspServer:: OnConnect(SOCKET sockfd)
{
    LOG_INFO("New RTSP connection established with sockfd: " + std::to_string(sockfd));

    auto conn = RtspConnection::Create(shared_from_this(),
                                       acceptor_->GetTaskScheduler().get(),
                                       sockfd);

    auto rtsp_conn = std::dynamic_pointer_cast<RtspConnection>(conn);
    if (!rtsp_conn)
    {
        LOG_ERROR("cast to RtspConnection failed, sockfd=" + std::to_string(sockfd));
        return conn;
    }

    std::shared_ptr<service::auth::RtspDigestAuthenticator> digest;
    if (auth_options_.enabled)
        digest = std::make_shared<service::auth::RtspDigestAuthenticator>(credentials_, auth_options_.realm,
            auth_options_.nonce_ttl_seconds, auth_options_.allow_legacy_digest);
    auto session = std::make_shared<rtsp::RtspSession>(rtsp_conn, std::move(digest));
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
