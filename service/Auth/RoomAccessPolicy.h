#ifndef PACKETIA_SERVICE_AUTH_ROOM_ACCESS_POLICY_H_
#define PACKETIA_SERVICE_AUTH_ROOM_ACCESS_POLICY_H_

#include "AuthAccessPolicy.h"

namespace service::auth
{

class RoomAccessPolicy final : public IAccessPolicy
{
public:
    AuthDecision Check(const AuthContext& caller, Action action,
                       const ResourceContext& resource) const override;
};

}

#endif // PACKETIA_SERVICE_AUTH_ROOM_ACCESS_POLICY_H_
