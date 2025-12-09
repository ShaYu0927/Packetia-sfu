#include "RtspServer.h"

RtspServer::RtspServer(EventLoop* event_loop)
    : TcpServer(event_loop)
{
    // Initialize the RTSP server
    LOG_INFO("RtspServer created with event loop: " + std::to_string(reinterpret_cast<uintptr_t>(event_loop)));

}

RtspServer::~RtspServer()
{
}

TcpConnection::Ptr RtspServer:: OnConnect(SOCKET sockfd)
{
    LOG_INFO("New RTSP connection established with sockfd: " + std::to_string(sockfd));
    auto conn = std::make_shared<RtspConnection>(shared_from_this(), event_loop_->GetTaskScheduler().get(), sockfd);

     conn->SetReadCallback([conn](TcpConnection::Ptr, BufferReader& buffer) {
        try {
            if (conn->GetConnectionType() == TcpConnection::ConnectionType::Rtsp) {
                auto rtsp_conn = std::dynamic_pointer_cast<RtspConnection>(conn);
                if (rtsp_conn) {
                    return rtsp_conn->onRead(buffer);
                }
            }
            // 其它类型连接的默认处理，或返回true忽略
            return true;
        } catch (const std::exception& e) {
            LOG_ERROR("Exception in RTSP read callback: " + std::string(e.what()));
            return false;
        }
    });

    conn->SetDisconnectCallback([this](TcpConnection::Ptr conn) {
        auto scheduler = conn->GetTaskScheduler();
        int socketfd = conn->GetSocket();
        LOG_INFO("Connection disconnected, scheduling removal for sockfd: " + std::to_string(socketfd));

        if (!scheduler->AddTriggerEvent([this, socketfd] { this->RemoveConnection(socketfd); })) {
            scheduler->AddTimer([this, socketfd]() {
                this->RemoveConnection(socketfd);
                return false;
            }, 100);
        }
    });
    return conn;
}