#ifndef _DEFAULTSESSIONFACTORY_H_
#define _DEFAULTSESSIONFACTORY_H_

#include "TcpSession.h"

class DefaultSessionFactory : public itcp_sess::ISessionFactory 
{
public:
    using Creator = std::function<itcp_sess::ISessionBase::Ptr(TcpConnection::Ptr)>;
    DefaultSessionFactory();

    itcp_sess::ISessionBase::Ptr Create(const std::string& proto,
                                        TcpConnection::Ptr conn) override;

    void Register(const std::string& proto, Creator c);

private:
    std::unordered_map<std::string, Creator> creators_;
};

#endif // _DEFAULTSESSIONFACTORY_H_