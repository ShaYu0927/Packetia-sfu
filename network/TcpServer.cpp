//
// Created by roots on 2024/9/11.
//

#include "TcpServer.h"



TcpServer::TcpServer(EventLoop *event_loop)
    : event_loop_(event_loop)
	, port_(0)
	, acceptor_(new Acceptor(event_loop_))
	, is_started_(false)
    , proto_detector_(std::make_shared<protocol::ProtocolDetector>())
{
    acceptor_->SetNewConnectionCallback([this](int sockfd) 
    {
        TcpConnection::Ptr conn = this->OnConnect(sockfd);
        if(!conn) return;

        this->AddConnection(sockfd, conn);

        auto promote = [this, sockfd](itcp_sess::ISessionBase::Ptr sess) {
            sessions_[sockfd] = std::move(sess);
        };

        auto det = std::make_shared<protocol::ProtocolDetectorSession>(proto_detector_, promote);
        if (sess_factory_)
        {
            det->SetSessionFactory(sess_factory_);
        }
        sessions_[sockfd] = det;

        conn->SetReadCallback([this,sockfd](TcpConnection::Ptr conn, BufferReader& buffer) 
        {
            auto it = sessions_.find(sockfd);
            if (it != sessions_.end()) 
            {
                it->second->OnRead(conn,buffer);
            }
            return true;
        });
    
       conn->SetDisconnectCallback([this](TcpConnection::Ptr conn)
       {
            auto scheduler = conn->GetTaskScheduler();
            int fd = conn->GetSocket();
            std::weak_ptr<TcpConnection> weak = conn;

            auto cleanup = [this, fd, weak]() 
            {
                auto itc = connections_.find(fd);
                if (itc == connections_.end()) return;

                auto sp = weak.lock();
                if (!sp || itc->second != sp) return;

                auto its = sessions_.find(fd);
                if (its != sessions_.end()) 
                {
                    its->second->OnClosed(0);
                    sessions_.erase(its);
                }

                RemoveConnection(fd);
            };

            if (!scheduler->AddTriggerEvent(cleanup)) 
            {
                scheduler->AddTimer([cleanup]() { cleanup(); return false; }, 100);
            }
        });
    });
}

TcpServer::~TcpServer()
{
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
            port_ = port;
            return true;
        }
    }

    return false;
}

void TcpServer::Stop()
{
   if (is_started_) 
   {		
		mutex_.lock();
		for (auto iter : connections_) 
        {
			iter.second->Disconnect();
		}
		mutex_.unlock();

		acceptor_->Close();
		is_started_ = false;

		while (1) 
        {
			std::this_thread::sleep_for(std::chrono::milliseconds(10));
			if (connections_.empty()) 
            {
				break;
			}
		}
	}	
}

TcpConnection::Ptr TcpServer::OnConnect(SOCKET sockfd)
{
    auto ts = event_loop_->GetTaskScheduler().get();
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


