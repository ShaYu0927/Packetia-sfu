#include "Acceptor.h"
#include "Socket.h"
#include "SocketUtil.h"
#include "logger.h"

Acceptor::Acceptor(EventLoop *eventLoop)
    : event_loop_(eventLoop)
    , tcp_socket_(new TcpSocket)
{

}

Acceptor::~Acceptor()
{
    Close();
}

int Acceptor::Listen(std::string ip, uint16_t port)
{
    Close();
    scheduler_ = event_loop_->GetTaskScheduler();
    if (!scheduler_ || scheduler_->IsStopped()) return -1;
    int result = -1;
    scheduler_->Invoke([&, this] {
    if(tcp_socket_->GetSocket() != INVALID_SOCKET)
    {
        tcp_socket_->Close();
    }
    SOCKET sockfd = tcp_socket_->Create();
    if(sockfd == INVALID_SOCKET)
    {
        return;
    }
    channel_ptr_.reset(new Channel(sockfd));
    SocketUtil::SetReuseAddr(sockfd);
	SocketUtil::SetReusePort(sockfd);
	SocketUtil::SetNonBlock(sockfd);

    if (!tcp_socket_->Bind(ip, port)) 
    {
        tcp_socket_->Close();
		return;
	}

    if (!tcp_socket_->Listen(1024)) 
    {
        tcp_socket_->Close();
		return;
	}
    channel_ptr_->SetReadCallback([this]() { this->OnAccept(); });
    sockaddr_in local{};
    socklen_t length = sizeof(local);
    if (::getsockname(sockfd, reinterpret_cast<sockaddr*>(&local), &length) == 0)
        port_ = ntohs(local.sin_port);
	channel_ptr_->EnableReading();
    LOG_INFO("EnableReading called on fd=" + std::to_string(tcp_socket_->GetSocket()));
	scheduler_->UpdateChannel(channel_ptr_);
    result = 0;
    });
    return result;
}

void Acceptor::Close()
{
    if (!scheduler_) return;
    scheduler_->Invoke([this] {
    if (tcp_socket_->GetSocket() >= 0)
    {
		if (channel_ptr_) scheduler_->RemoveChannel(channel_ptr_);
		tcp_socket_->Close();
	}
    channel_ptr_.reset();
    port_ = 0;
    });
}

void Acceptor::OnAccept()
{
    while (true) 
    {
        int sockfd = tcp_socket_->Accept();
        if (sockfd >= 0)
        {
            if (new_connection_callback_) new_connection_callback_(sockfd);
            else SocketUtil::Close(sockfd);
            continue;
        }
        if (errno == EINTR) continue;
        if (errno == EAGAIN || errno == EWOULDBLOCK) break;
        LOG_ERROR("accept failed, errno=" + std::to_string(errno));
        break; 
    }
}
