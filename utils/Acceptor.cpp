#include "Acceptor.h"
#include "Socket.h"

Acceptor::Acceptor(EventLoop *eventLoop)
    : event_loop_(eventLoop)
    , tcp_socket_(new TcpSocket)
{

}

Acceptor::~Acceptor()
{
}

int Acceptor::Listen(std::string ip, uint16_t port)
{
    std::lock_guard<std::mutex> locker(mutex_);
    if(tcp_socket_->GetSocket() == INVALID_SOCKET)
    {
        tcp_socket_->Close();
    }
    SOCKET sockfd = tcp_socket_->Create();
    if(sockfd == INVALID_SOCKET)
    {
        return -1;
    }
    channel_ptr_.reset(new Channel(sockfd));
    SocketUtil::SetReuseAddr(sockfd);
	SocketUtil::SetReusePort(sockfd);
	SocketUtil::SetNonBlock(sockfd);

    if (!tcp_socket_->Bind(ip, port)) 
    {
		return -1;
	}

    if (!tcp_socket_->Listen(1024)) 
    {
		return -1;
	}
    channel_ptr_->SetReadCallback([this]() { this->OnAccept(); }); //有新连接，触发回调函数
	channel_ptr_->EnableReading();
	event_loop_->UpdateChannel(channel_ptr_);
    return 0;
}

void Acceptor::Close()
{
    std::lock_guard<std::mutex> locker(mutex_);
    if (tcp_socket_->GetSocket() > 0) 
    {
		event_loop_->RemoveChannel(channel_ptr_);
		tcp_socket_->Close();
	}
}

void Acceptor::OnAccept()
{
    std::lock_guard<std::mutex> locker(mutex_);
    int sockfd = tcp_socket_->Accept();
    std::cout << "OnAccept called, accept fd: " << sockfd << std::endl;
    if(sockfd > 0)
    {
        
        if (new_connection_callback_) 
        {
            std::cout << "[Acceptor] OnAccept success, connfd: " << sockfd << std::endl;
            new_connection_callback_(sockfd);
        }
        else
        {
            SocketUtil::Close(sockfd);
        }
    }
}
