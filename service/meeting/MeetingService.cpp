#include "MeetingService.h"
#include "ClusterStore.h"
#include "Coordinator.h"
namespace service::meeting
{
using cluster::Failure;
namespace
{
void RequireId(const std::string& value)
{ if (!cluster::Coordinator::ValidId(value)) throw Failure(400, "invalid_id"); }
}
std::string MeetingService::CreateRoom(const std::string& owner)
{
    RequireId(owner);
    cluster::ClusterStore::Write transaction(*store_);
    auto count = transaction.Prepare("SELECT COUNT(*) FROM cluster_rooms WHERE owner=? AND state<>'ended'");
    count.Text(1, owner); count.Step();
    if (count.Number(0) >= 100) throw Failure(409, "room_quota_exceeded");
    const auto room = cluster::Coordinator::RandomId();
    auto create = transaction.Prepare("INSERT INTO cluster_rooms(id,owner) VALUES(?,?)");
    create.Text(1, room).Text(2, owner).Step();
    auto member = transaction.Prepare("INSERT INTO cluster_members(room,user,publish) VALUES(?,?,1)");
    member.Text(1, room).Text(2, owner).Step();
    transaction.Commit();
    return room;
}
void MeetingService::RequireOwner(const std::string& room, const std::string& owner, bool allow_ended)
{
    RequireId(room); RequireId(owner);
    cluster::ClusterStore::Write transaction(*store_);
    auto query = transaction.Prepare("SELECT owner,state FROM cluster_rooms WHERE id=?");
    query.Text(1, room);
    if (!query.Step() || query.Text(0) != owner) throw Failure(403, "room_owner_required");
    if (!allow_ended && query.Text(1) != "open") throw Failure(410, "room_ended");
    transaction.Commit();
}
void MeetingService::SetMemberRole(const std::string& room, const std::string& owner, const std::string& user, bool can_publish)
{
    RequireId(room); RequireId(owner); RequireId(user);
    cluster::ClusterStore::Write transaction(*store_);
    auto query = transaction.Prepare("SELECT owner,state FROM cluster_rooms WHERE id=?");
    query.Text(1, room);
    if (!query.Step() || query.Text(0) != owner) throw Failure(403, "room_owner_required");
    if (query.Text(1) != "open") throw Failure(410, "room_ended");
    if (user == owner && !can_publish) throw Failure(400, "owner_role_required");
    auto count = transaction.Prepare("SELECT COUNT(*) FROM cluster_members WHERE room=? AND user<>?");
    count.Text(1, room).Text(2, user); count.Step();
    if (count.Number(0) >= 1000) throw Failure(409, "member_quota_exceeded");
    auto insert = transaction.Prepare("INSERT INTO cluster_members(room,user,publish) VALUES(?,?,?) "
        "ON CONFLICT(room,user) DO UPDATE SET publish=excluded.publish");
    insert.Text(1, room).Text(2, user).Number(3, can_publish).Step();
    transaction.Commit();
}
MemberGrant MeetingService::Grant(const std::string& room, const std::string& user)
{
    RequireId(room); RequireId(user);
    cluster::ClusterStore::Write transaction(*store_);
    auto query = transaction.Prepare("SELECT m.publish,r.owner,r.state FROM cluster_members m JOIN cluster_rooms r ON r.id=m.room WHERE m.room=? AND m.user=?");
    query.Text(1, room).Text(2, user);
    if (!query.Step()) throw Failure(403, "room_membership_required");
    if (query.Text(2) != "open") throw Failure(410, "room_ended");
    MemberGrant result{query.Number(0) != 0, query.Text(1) == user};
    transaction.Commit();
    return result;
}
bool MeetingService::BeginEnd(const std::string& room, const std::string& owner)
{
    RequireId(room); RequireId(owner);
    cluster::ClusterStore::Write transaction(*store_);
    auto query = transaction.Prepare("SELECT owner,state,node FROM cluster_rooms WHERE id=?");
    query.Text(1, room);
    if (!query.Step() || query.Text(0) != owner) throw Failure(403, "room_owner_required");
    const bool ended = query.Text(1) == "ended" || query.Text(2).empty();
    auto update = transaction.Prepare("UPDATE cluster_rooms SET state=? WHERE id=?");
    update.Text(1, ended ? "ended" : "closing").Text(2, room).Step();
    transaction.Commit();
    return ended;
}
}
