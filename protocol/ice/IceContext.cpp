#include "IceContext.h"

#include <utility>

void IceContext::SetLocalCredentials(std::string ufrag, std::string pwd)
{
    local_ufrag_ = std::move(ufrag);
    local_pwd_ = std::move(pwd);
}

void IceContext::SetRemoteCredentials(std::string ufrag, std::string pwd)
{
    remote_ufrag_ = std::move(ufrag);
    remote_pwd_ = std::move(pwd);
}
