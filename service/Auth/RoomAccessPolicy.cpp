#include "RoomAccessPolicy.h"

namespace service::auth
{

AuthDecision RoomAccessPolicy::Check(const AuthContext& caller, Action action,
                                    const ResourceContext& resource) const
{
    if (caller.audience != Audience::WebRtc || caller.subject_id.empty() ||
        !caller.video.room_join || caller.video.room.empty())
        return AuthDecision::Deny();

    const auto& room = resource.type == ResourceType::Room ? resource.id : resource.room_id;
    if ((resource.type != ResourceType::Room && resource.type != ResourceType::Track) ||
        room.empty() || room != caller.video.room ||
        (resource.type == ResourceType::Room && !resource.room_id.empty() && resource.room_id != room))
        return AuthDecision::Deny();

    switch (action)
    {
    case Action::JoinRoom:
    case Action::ListTracks:
        return resource.type == ResourceType::Room ? AuthDecision::Allow() : AuthDecision::Deny();
    case Action::Publish:
        return caller.video.can_publish ? AuthDecision::Allow() : AuthDecision::Deny();
    case Action::Subscribe:
        return caller.video.can_subscribe ? AuthDecision::Allow() : AuthDecision::Deny();
    default:
        return AuthDecision::Deny();
    }
}

}
