//
// Created by roots on 2024/9/11.
//

#include "TcpServer.h"

#include "SipParse.h"


TcpServer::TcpServer(EventLoop *event_loop)
    : event_loop_(event_loop)
	, port_(0)
	, acceptor_(new Acceptor(event_loop_))
	, is_started_(false)
{
    acceptor_->SetNewConnectionCallback([this](int sockfd) 
    {
        LOG_INFO("New connection accepted: sockfd = " + std::to_string(sockfd));
        auto detector = std::make_shared<ProtocolDetector>();
        // detector->Register({
        //     "RTSP",
        //     4,
        //     [](const uint8_t* data, size_t size) {
        //         const char* prefix = "RTSP";
        //         return size >= 4 && std::equal(prefix, prefix + 4, reinterpret_cast<const char*>(data));
        //     },
        //     []() { return std::make_shared<RtspParse>(); }
        // });

        detector->Register({
            "SIP",
            3,
            [](const uint8_t* data, size_t size) {
                const char* prefix1 = "INV";
                const char* prefix2 = "REG";
                return size >= 3 && (std::equal(prefix1, prefix1 + 3, reinterpret_cast<const char*>(data)) ||
                                     std::equal(prefix2, prefix2 + 3, reinterpret_cast<const char*>(data)));
            },
            []() { return std::make_shared<SipParse>(); }
        });
        TcpConnection::Ptr conn = this->OnConnect(sockfd);
        if(conn)
        {
            this->AddConnection(sockfd, conn);
            if (dynamic_cast<RtspConnection*>(conn.get()) == nullptr) 
            {
                conn->SetReadCallback([](TcpConnection::Ptr conn, BufferReader& buffer) {
                    std::string msg(buffer.Peek(), buffer.ReadableBytes());
                    LOG_INFO("[业务] 收到消息: " + msg);
                    return true;
                    });
            }
            conn->SetDisconnectCallback([this](TcpConnection::Ptr conn){
                auto scheduler = conn->GetTaskScheduler();
                int socketfd = conn->GetSocket();
                //用于管理异步任务和定时任务
                if (!scheduler->AddTriggerEvent([this, socketfd] {this->RemoveConnection(socketfd); })) {
					scheduler->AddTimer([this, socketfd]() {this->RemoveConnection(socketfd); return false; }, 100);
				}   
            });

             conn->SetProtocolDetector(detector);


        }
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
    return std::make_shared<TcpConnection>(event_loop_->GetTaskScheduler().get(), sockfd);
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
