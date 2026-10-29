#include "Coordinator.h"
#include "ClusterStore.h"
#include <sqlite3.h>
#include <openssl/rand.h>
#include <chrono>
#include <utility>

namespace service::cluster
{
namespace
{
void RequireId(const std::string& value)
{
    if (!Coordinator::ValidId(value)) throw Failure(400, "invalid_id");
}
}

int64_t Coordinator::UnixSeconds()
{
    return std::chrono::duration_cast<std::chrono::seconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();
}
std::string Coordinator::RandomId()
{
    unsigned char data[16];
    if (RAND_bytes(data, sizeof(data)) != 1) throw Failure(503, "random_unavailable");
    const char* digits = "0123456789abcdef";
    std::string id;
    for (auto c : data) { id += digits[c >> 4]; id += digits[c & 15]; }
    return id;
}
bool Coordinator::ValidId(const std::string& value)
{
    if (value.empty() || value.size() > 128) return false;
    for (unsigned char c : value)
        if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
              (c >= '0' && c <= '9') || c == '_' || c == '-' || c == '.')) return false;
    return true;
}
bool Coordinator::ValidSignalingUrl(const std::string& value)
{
    std::size_t prefix = value.rfind("ws://", 0) == 0 ? 5 : value.rfind("wss://", 0) == 0 ? 6 : 0;
    if (!prefix || value.size() <= prefix || value.size() > 1024) return false;
    for (unsigned char c : value) if (c <= 32 || c >= 127 || c == '@' || c == '#' || c == '?') return false;
    return value[prefix] != '/' && value[prefix] != ':';
}
bool Coordinator::ValidManagementUrl(const std::string& value)
{
    const auto prefix = value.rfind("http://", 0) == 0 ? 7 : value.rfind("https://", 0) == 0 ? 8 : 0;
    if (!prefix || value.size() <= prefix || value.size() > 1024 || value.back() == '/') return false;
    for (unsigned char c : value) if (c <= 32 || c >= 127 || c == '@' || c == '#' || c == '?') return false;
    return value[prefix] != '/' && value[prefix] != ':';
}
Coordinator::Coordinator(std::shared_ptr<ClusterStore> store, Clock clock)
    : store_(std::move(store)), clock_(clock ? std::move(clock) : Clock{UnixSeconds}) {}
Coordinator::Coordinator(const std::string& path, Clock clock)
    : Coordinator(std::make_shared<ClusterStore>(path), std::move(clock)) {}
Coordinator::~Coordinator() = default;
void Coordinator::Register(const Node& node)
{
    RequireId(node.id); RequireId(node.instance);
    if (!ValidSignalingUrl(node.signaling_url) || (!node.management_url.empty() && !ValidManagementUrl(node.management_url)) || node.max_rooms < 1 || node.max_rooms > 10000 || node.participants < 0)
        throw Failure(400, "invalid_node");
    ClusterStore::Write transaction(*store_);
    const auto now = clock_();
    auto existing = transaction.Prepare("SELECT instance,url,capacity,expires,management_url FROM cluster_nodes WHERE id=?");
    existing.Text(1, node.id);
    const bool found = existing.Step();
    if (found && existing.Text(0) == node.instance)
    {
        if (existing.Text(1) != node.signaling_url || existing.Number(2) != node.max_rooms || existing.Text(4) != node.management_url)
            throw Failure(409, "instance_configuration_changed");
    }
    else
    {
        if (found && existing.Number(3) > now) throw Failure(409, "node_already_live");
        auto retired = transaction.Prepare("SELECT 1 FROM cluster_instances WHERE node=? AND instance=?");
        retired.Text(1, node.id).Text(2, node.instance);
        if (retired.Step()) throw Failure(409, "instance_retired");
        auto remember = transaction.Prepare("INSERT INTO cluster_instances(node,instance) VALUES(?,?)");
        remember.Text(1, node.id).Text(2, node.instance).Step();
    }
    auto update = transaction.Prepare("INSERT INTO cluster_nodes(id,instance,url,capacity,expires,participants,management_url) VALUES(?,?,?,?,?,?,?) "
        "ON CONFLICT(id) DO UPDATE SET instance=excluded.instance,url=excluded.url,capacity=excluded.capacity,"
        "expires=excluded.expires,participants=excluded.participants,management_url=excluded.management_url");
    update.Text(1, node.id).Text(2, node.instance).Text(3, node.signaling_url).Number(4, node.max_rooms)
        .Number(5, now + kLeaseSeconds).Number(6, node.participants).Text(7, node.management_url).Step();
    transaction.Commit();
}
void Coordinator::Heartbeat(const std::string& node, const std::string& instance, int participants)
{
    RequireId(node); RequireId(instance);
    if (participants < 0 || participants > 65536) throw Failure(400, "invalid_load");
    ClusterStore::Write transaction(*store_);
    auto update = transaction.Prepare("UPDATE cluster_nodes SET expires=?,participants=? WHERE id=? AND instance=?");
    update.Number(1, clock_() + kLeaseSeconds).Number(2, participants).Text(3, node).Text(4, instance).Step();
    if (transaction.Changes() != 1) throw Failure(409, "instance_fenced");
    transaction.Commit();
}
namespace
{
Node ReadNode(ClusterStore::Write& transaction, const std::string& node, const std::string& instance, int64_t now)
{
    auto query = transaction.Prepare("SELECT id,instance,url,capacity,participants,management_url FROM cluster_nodes WHERE id=? AND instance=? AND expires>?");
    query.Text(1, node).Text(2, instance).Number(3, now);
    if (!query.Step()) throw Failure(503, "room_node_unavailable");
    return {query.Text(0), query.Text(1), query.Text(2), static_cast<int>(query.Number(3)),
        static_cast<int>(query.Number(4)), query.Text(5)};
}
}
RoomAssignment Coordinator::AssignRoom(const std::string& room)
{
    RequireId(room);
    ClusterStore::Write transaction(*store_);
    auto binding = transaction.Prepare("SELECT node,instance,assignment_id,state FROM cluster_rooms WHERE id=?");
    binding.Text(1, room);
    if (!binding.Step()) throw Failure(404, "room_not_found");
    if (binding.Text(3) != "open") throw Failure(410, "room_ended");
    const auto now = clock_();
    RoomAssignment assignment;
    assignment.room_id = room;
    assignment.assignment_id = binding.Text(2);
    if (!binding.Text(0).empty()) assignment.node = ReadNode(transaction, binding.Text(0), binding.Text(1), now);
    else
    {
        auto query = transaction.Prepare("SELECT n.id,n.instance,n.url,n.capacity,n.participants,n.management_url,COUNT(r.id) AS assigned "
            "FROM cluster_nodes n LEFT JOIN cluster_rooms r ON r.node=n.id AND r.instance=n.instance "
            "WHERE n.expires>? GROUP BY n.id HAVING assigned<n.capacity "
            "ORDER BY CAST(assigned AS REAL)/n.capacity,n.participants,n.id LIMIT 1");
        query.Number(1, now);
        if (!query.Step()) throw Failure(503, "no_available_node");
        assignment.node = {query.Text(0), query.Text(1), query.Text(2), static_cast<int>(query.Number(3)),
            static_cast<int>(query.Number(4)), query.Text(5)};
        assignment.assignment_id = RandomId();
    }
    assignment.reclaim_after = now + kAdmissionHoldSeconds;
    auto update = transaction.Prepare("UPDATE cluster_rooms SET node=?,instance=?,assignment_id=?,reclaim_after=? WHERE id=? AND state='open'");
    update.Text(1, assignment.node.id).Text(2, assignment.node.instance).Text(3, assignment.assignment_id)
        .Number(4, assignment.reclaim_after).Text(5, room).Step();
    transaction.Commit();
    return assignment;
}
std::optional<RoomAssignment> Coordinator::FindRoom(const std::string& room, bool closing)
{
    RequireId(room);
    ClusterStore::Write transaction(*store_);
    auto binding = transaction.Prepare("SELECT node,instance,assignment_id,reclaim_after,state FROM cluster_rooms WHERE id=?");
    binding.Text(1, room);
    if (!binding.Step()) throw Failure(404, "room_not_found");
    if (binding.Text(4) != "open" && !(closing && binding.Text(4) == "closing")) throw Failure(410, "room_ended");
    if (binding.Text(0).empty()) { transaction.Commit(); return {}; }
    RoomAssignment result{room, binding.Text(2), ReadNode(transaction, binding.Text(0), binding.Text(1), clock_()),
        binding.Number(3), binding.Text(4) == "closing"};
    transaction.Commit();
    return result;
}
std::vector<RoomAssignment> Coordinator::ReclaimCandidates(const std::string& node, const std::string& instance)
{
    RequireId(node); RequireId(instance);
    ClusterStore::Write transaction(*store_);
    ReadNode(transaction, node, instance, clock_());
    auto query = transaction.Prepare("SELECT id,assignment_id,reclaim_after,state FROM cluster_rooms "
        "WHERE node=? AND instance=? AND (state='closing' OR reclaim_after<=?) ORDER BY last_probe,id LIMIT 16");
    query.Text(1, node).Text(2, instance).Number(3, clock_());
    std::vector<RoomAssignment> result;
    while (query.Step()) result.push_back({query.Text(0), query.Text(1), {}, query.Number(2), query.Text(3) == "closing"});
    for (const auto& item : result)
    {
        auto update = transaction.Prepare("UPDATE cluster_rooms SET last_probe=? WHERE id=?");
        update.Number(1, clock_()).Text(2, item.room_id).Step();
    }
    transaction.Commit();
    return result;
}
bool Coordinator::ReleaseRoom(const std::string& node, const std::string& instance,
                              const std::string& room, const std::string& assignment)
{
    RequireId(node); RequireId(instance); RequireId(room); RequireId(assignment);
    ClusterStore::Write transaction(*store_);
    ReadNode(transaction, node, instance, clock_());
    // 节点确认 owner 已停止接纳该代次后才清除绑定；迟到回收不能清除新绑定。
    auto update = transaction.Prepare("UPDATE cluster_rooms SET node=NULL,instance=NULL,assignment_id='',reclaim_after=0,last_probe=0,"
        "state=CASE WHEN state='closing' THEN 'ended' ELSE state END WHERE id=? AND node=? AND instance=? AND assignment_id=?");
    update.Text(1, room).Text(2, node).Text(3, instance).Text(4, assignment).Step();
    const bool changed = transaction.Changes() == 1;
    transaction.Commit();
    return changed;
}
}
