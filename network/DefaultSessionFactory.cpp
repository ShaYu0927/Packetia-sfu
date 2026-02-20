#include "DefaultSessionFactory.h"


DefaultSessionFactory::DefaultSessionFactory()
{
    
}

itcp_sess::ISessionBase::Ptr
DefaultSessionFactory::Create(const std::string& proto, TcpConnection::Ptr conn)
{
    auto it = creators_.find(proto);
        if (it == creators_.end()) return nullptr;
        return it->second(std::move(conn));
    return nullptr;
}


void DefaultSessionFactory::Register(const std::string& proto, Creator c)
{
    creators_[proto] = std::move(c);
}