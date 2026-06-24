#include "SipServer.h"

#include "DefaultSessionFactory.h"
#include "SipSession.h"
#include "logger.h"

SipServer::SipServer(EventLoop* event_loop)
    : TcpServer(event_loop)
{
    LOG_INFO("SipServer created with event loop: ", reinterpret_cast<uintptr_t>(event_loop));

    auto factory = std::make_shared<DefaultSessionFactory>();
    factory->Register("SIP",
        [](TcpConnection::Ptr conn) -> itcp_sess::ISessionBase::Ptr {
            return std::make_shared<sip::SipSession>(std::move(conn));
        });

    SetSessionFactory(factory);
}

SipServer::~SipServer() = default;

TcpConnection::Ptr SipServer::OnConnect(SOCKET sockfd)
{
    LOG_INFO("New SIP connection established, sockfd=", sockfd);
    return TcpServer::OnConnect(sockfd);
}