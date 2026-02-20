#include "RtspServer.h"
#include "RtspSession.h"

RtspServer::RtspServer(EventLoop* event_loop)
    : TcpServer(event_loop)
{
    // Initialize the RTSP server
    LOG_INFO("RtspServer created with event loop: " + std::to_string(reinterpret_cast<uintptr_t>(event_loop)));

    auto factory = std::make_shared<DefaultSessionFactory>();

     factory->Register("RTSP",
        [](TcpConnection::Ptr conn) -> itcp_sess::ISessionBase::Ptr {
            return std::make_shared<rtsp::RtspSession>(std::move(conn));
        });

    SetSessionFactory(factory);
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

    std::weak_ptr<RtspServer> weak_server = shared_from_this();

    conn->SetDisconnectCallback([weak_server](TcpConnection::Ptr conn) {
        auto server = weak_server.lock();
        if (!server) return;

        auto scheduler = conn->GetTaskScheduler();
        int socketfd = conn->GetSocket();

        LOG_INFO("Connection disconnected, scheduling removal for sockfd: " + std::to_string(socketfd));

        if (!scheduler->AddTriggerEvent([weak_server, socketfd] {
                if (auto s = weak_server.lock()) s->RemoveConnection(socketfd);
            }))
        {
            scheduler->AddTimer([weak_server, socketfd]() {
                if (auto s = weak_server.lock()) s->RemoveConnection(socketfd);
                return false;
            }, 100);
        }
    });

    return conn;
}