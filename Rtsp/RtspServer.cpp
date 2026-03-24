#include "RtspServer.h"
#include "RtspSession.h"
#include "DefaultSessionFactory.h"


RtspServer::RtspServer(EventLoop* event_loop)
    : TcpServer(event_loop)
{
    LOG_INFO("RtspServer created with event loop: " + std::to_string(reinterpret_cast<uintptr_t>(event_loop)));

    auto factory = std::make_shared<DefaultSessionFactory>();

    factory->Register("RTSP",
        [](TcpConnection::Ptr conn) -> itcp_sess::ISessionBase::Ptr {
            auto rtsp_conn = std::dynamic_pointer_cast<RtspConnection>(conn);
            if (!rtsp_conn)
            {
                LOG_ERROR("session factory cast TcpConnection -> RtspConnection failed");
                return nullptr;
            }
            return std::make_shared<rtsp::RtspSession>(std::move(rtsp_conn));
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
    auto rtsp_conn = std::dynamic_pointer_cast<RtspConnection>(conn);
    if (!rtsp_conn)
    {
        LOG_ERROR("cast to RtspConnection failed, sockfd=" + std::to_string(sockfd));
        return conn;
    }

    auto session = std::make_shared<rtsp::RtspSession>(rtsp_conn);
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
    conn->Start();
    return conn;
}