#pragma once
#include "AuthAccessPolicy.h"

namespace service::auth
{
class StreamAccessPolicy final : public IAccessPolicy
{
public:
    AuthDecision Check(const AuthContext& caller, Action action, const ResourceContext& resource) const override;
};
}
