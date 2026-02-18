#ifndef _DEFAULTSESSIONFACTORY_H_
#define _DEFAULTSESSIONFACTORY_H_

#include "TcpSession.h"
#include "RtspServer.h"

class DefaultSessionFactory : public itcp_sess::ISessionFactory 
{
public:
    DefaultSessionFactory(std::shared_ptr<RtspServer> rtsp_server,
                          TaskScheduler* scheduler);

    itcp_sess::ISessionBase::Ptr Create(const std::string& proto,
                                        TcpConnection::Ptr conn) override;

private:
    std::shared_ptr<RtspServer> rtsp_server_;
    TaskScheduler* scheduler_{nullptr};
};

#endif // _DEFAULTSESSIONFACTORY_H_