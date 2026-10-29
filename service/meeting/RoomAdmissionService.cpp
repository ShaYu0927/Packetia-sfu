#include "RoomAdmissionService.h"
#include "JoinToken.h"
namespace service::meeting
{
RoomAdmission RoomAdmissionService::Admit(const std::string& room, const std::string& user, const std::string& name)
{
    const auto grant = meetings_->Grant(room, user);
    const auto assignment = nodes_->AssignRoom(room);
    auth::AuthContext context;
    context.subject_id = user; context.name = name; context.issuer = issuer_;
    context.token_id = cluster::Coordinator::RandomId();
    const auto now = cluster::Coordinator::UnixSeconds();
    context.expires_at_unix_seconds = now + 60;
    context.video = {room, true, grant.can_publish, true, grant.room_admin, true};
    context.node_id = assignment.node.id; context.instance_id = assignment.node.instance;
    context.assignment_id = assignment.assignment_id;
    return {assignment, auth::SignJoinToken(context, secret_, now), context.expires_at_unix_seconds};
}
}
