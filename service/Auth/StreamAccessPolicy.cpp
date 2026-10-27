#include "StreamAccessPolicy.h"
#include "RtspAuthOptions.h"

namespace service::auth
{
AuthDecision StreamAccessPolicy::Check(const AuthContext& caller, Action action, const ResourceContext& resource) const
{
    if (caller.audience != Audience::Rtsp || caller.subject_id.empty() || resource.type != ResourceType::Stream ||
        !IsValidStreamPath(resource.id) || (!resource.stream_id.empty() && resource.stream_id != resource.id))
        return AuthDecision::Deny();
    for (const auto& grant : caller.streams)
        if (grant.path == resource.id)
            return (action == Action::Publish && grant.can_publish) || (action == Action::Subscribe && grant.can_play)
                ? AuthDecision::Allow() : AuthDecision::Deny();
    return AuthDecision::Deny();
}
}
