#ifndef PACKETIA_SERVICE_AUTH_ACCESS_POLICY_H_
#define PACKETIA_SERVICE_AUTH_ACCESS_POLICY_H_

#include "AuthTypes.h"

namespace service::auth
{

class IAccessPolicy
{
public:
    virtual ~IAccessPolicy() = default;

    virtual AuthDecision Check(const AuthContext& caller, Action action, const ResourceContext& resource) const = 0;
};

}

#endif // PACKETIA_SERVICE_AUTH_ACCESS_POLICY_H_
