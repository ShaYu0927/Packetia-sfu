#include "DefaultSessionFactory.h"
#include "RtspSession.h"


DefaultSessionFactory::DefaultSessionFactory(std::shared_ptr<RtspServer> rtsp_server,
                                             TaskScheduler* scheduler)
    : rtsp_server_(std::move(rtsp_server))
    , scheduler_(scheduler)
{}

itcp_sess::ISessionBase::Ptr
DefaultSessionFactory::Create(const std::string& proto, TcpConnection::Ptr conn)
{
    if (proto == "RTSP")
        return std::make_shared<rtsp::RtspSession>(rtsp_server_, scheduler_, conn);

    return nullptr;
}