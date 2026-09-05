//
// Created by roots on 2024/9/11.
//

#include "TcpServer.h"



TcpServer::TcpServer(EventLoop *event_loop)
    : event_loop_(event_loop)
	, port_(0)
	, acceptor_(new Acceptor(event_loop_))
	, is_started_(false)
{
    acceptor_->SetNewConnectionCallback([this](int sockfd) 
    {
        TcpConnection::Ptr conn = this->OnConnect(sockfd);
        if(!conn) return;

        this->AddConnection(sockfd, conn);

       conn->SetDisconnectCallback([this](TcpConnection::Ptr conn)
       {
            int fd = conn->GetSocket();
            // Connections and the acceptor belong to the same scheduler.
            // No delayed callback may outlive this server.
            RemoveConnection(fd);
        });
    });
}

TcpServer::~TcpServer()
{
    Stop();
}

bool TcpServer::Start(std::string ip, uint16_t port)
{
    Stop();

    if(!is_started_)
    {
        if(acceptor_->Listen(ip,port) == 0)
        {
            is_started_ = true;
            ip_ = ip;
            port_ = acceptor_->GetPort();
            return true;
        }
    }

    return false;
}

void TcpServer::Stop()
{
    auto scheduler = acceptor_->GetTaskScheduler();
    if (!scheduler) return;
    scheduler->Invoke([this] {
        acceptor_->Close();
        is_started_ = false;
        std::vector<TcpConnection::Ptr> snapshot;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            for (const auto& entry : connections_) snapshot.push_back(entry.second);
        }
        // Disconnect invokes removal callbacks; do not hold the map lock.
        for (auto& connection : snapshot) connection->Disconnect();
        std::lock_guard<std::mutex> lock(mutex_);
        connections_.clear();
    });
}

TcpConnection::Ptr TcpServer::OnConnect(SOCKET sockfd)
{
    auto ts = acceptor_->GetTaskScheduler().get();
    auto conn = std::make_shared<TcpConnection>(ts, sockfd);
    conn->Start();              
    return conn;
}

void TcpServer::AddConnection(SOCKET sockfd, TcpConnection::Ptr tcp_conn)
{
    std::lock_guard<std::mutex> locker(mutex_);
	connections_.emplace(sockfd, tcp_conn);
}

void TcpServer::RemoveConnection(SOCKET sockfd)
{
    std::lock_guard<std::mutex> locker(mutex_);
	connections_.erase(sockfd);
}
