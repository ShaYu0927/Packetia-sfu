#include "rtmp_server.h"

#include <utility>

#include "logger.h"

namespace protocol::rtmp {

RtmpServer::RtmpServer(EventLoop* event_loop) : TcpServer(event_loop)
{
}

void RtmpServer::SetHandler(std::shared_ptr<IRtmpMessageHandler> handler)
{
    handler_ = std::move(handler);
    for (auto& entry : sessions_) entry.second->SetHandler(handler_);
}

TcpConnection::Ptr RtmpServer::OnConnect(SOCKET sockfd)
{
    LOG_INFO("New RTMP connection established, sockfd=", sockfd);
    auto connection = std::make_shared<TcpConnection>(acceptor_->GetTaskScheduler().get(), sockfd);
    auto session = std::make_shared<RtmpSession>(connection, EndpointRole::kServer);
    session->SetHandler(handler_);
    sessions_[sockfd] = session;
    session->Start();
    return connection;
}

void RtmpServer::RemoveConnection(SOCKET sockfd)
{
    auto it = sessions_.find(sockfd);
    if (it != sessions_.end()) {
        sessions_.erase(it);
    }
    TcpServer::RemoveConnection(sockfd);
}

}  // namespace protocol::rtmp
