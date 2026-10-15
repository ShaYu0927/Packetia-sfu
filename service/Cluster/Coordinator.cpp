#include "Coordinator.h"
#include <sqlite3.h>
#include <openssl/rand.h>
#include <chrono>
#include <utility>

namespace service::cluster
{
namespace
{
void Check(int result)
{
    if (result != SQLITE_OK && result != SQLITE_DONE && result != SQLITE_ROW)
        throw Failure(503, "storage_unavailable");
}
void Exec(sqlite3* db, const char* sql) { Check(sqlite3_exec(db, sql, nullptr, nullptr, nullptr)); }
class Query
{
public:
    Query(sqlite3* db, const char* sql) { Check(sqlite3_prepare_v2(db, sql, -1, &stmt_, nullptr)); }
    ~Query() { sqlite3_finalize(stmt_); }
    Query(const Query&) = delete;
    Query& operator=(const Query&) = delete;
    Query& Text(int index, const std::string& value)
    {
        Check(sqlite3_bind_text(stmt_, index, value.data(), static_cast<int>(value.size()), SQLITE_TRANSIENT));
        return *this;
    }
    Query& Number(int index, int64_t value) { Check(sqlite3_bind_int64(stmt_, index, value)); return *this; }
    bool Step() { const auto result = sqlite3_step(stmt_); Check(result); return result == SQLITE_ROW; }
    std::string Text(int index) const
    {
        const auto* value = sqlite3_column_text(stmt_, index);
        return value ? reinterpret_cast<const char*>(value) : "";
    }
    int64_t Number(int index) const { return sqlite3_column_int64(stmt_, index); }
private:
    sqlite3_stmt* stmt_ = nullptr;
};
class Transaction
{
public:
    explicit Transaction(sqlite3* db) : db_(db) { Exec(db_, "BEGIN IMMEDIATE"); }
    ~Transaction() { if (!committed_) sqlite3_exec(db_, "ROLLBACK", nullptr, nullptr, nullptr); }
    void Commit() { Exec(db_, "COMMIT"); committed_ = true; }
private:
    sqlite3* db_;
    bool committed_ = false;
};
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
Coordinator::Coordinator(const std::string& database_path, Clock clock)
    : clock_(clock ? std::move(clock) : Clock{UnixSeconds})
{
    try
    {
        Check(sqlite3_open_v2(database_path.c_str(), &db_, SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE | SQLITE_OPEN_FULLMUTEX, nullptr));
        Check(sqlite3_busy_timeout(db_, 5000));
        Exec(db_, "PRAGMA journal_mode=WAL; PRAGMA synchronous=FULL; PRAGMA foreign_keys=ON;");
        Exec(db_, "CREATE TABLE IF NOT EXISTS cluster_nodes("
            "id TEXT PRIMARY KEY, instance TEXT NOT NULL, url TEXT NOT NULL, capacity INTEGER NOT NULL,"
            "expires INTEGER NOT NULL, participants INTEGER NOT NULL);"
            "CREATE TABLE IF NOT EXISTS cluster_instances(node TEXT NOT NULL, instance TEXT NOT NULL, PRIMARY KEY(node,instance));"
            "CREATE TABLE IF NOT EXISTS cluster_rooms(id TEXT PRIMARY KEY, owner TEXT NOT NULL, node TEXT, instance TEXT);"
            "CREATE TABLE IF NOT EXISTS cluster_members(room TEXT NOT NULL REFERENCES cluster_rooms(id),"
            "user TEXT NOT NULL, publish INTEGER NOT NULL, PRIMARY KEY(room,user));"
            "CREATE INDEX IF NOT EXISTS cluster_room_binding ON cluster_rooms(node,instance);");
    }
    catch (...) { if (db_) sqlite3_close(db_); db_ = nullptr; throw; }
}
Coordinator::~Coordinator() { if (db_) sqlite3_close(db_); }

void Coordinator::Register(const Node& node)
{
    RequireId(node.id); RequireId(node.instance);
    if (!ValidSignalingUrl(node.signaling_url) || node.max_rooms < 1 || node.max_rooms > 10000 || node.participants < 0)
        throw Failure(400, "invalid_node");
    std::lock_guard<std::mutex> lock(mutex_);
    Transaction transaction(db_);
    const auto now = clock_();
    Query existing(db_, "SELECT instance,url,capacity,expires FROM cluster_nodes WHERE id=?");
    existing.Text(1, node.id);
    const bool found = existing.Step();
    if (found && existing.Text(0) == node.instance)
    {
        if (existing.Text(1) != node.signaling_url || existing.Number(2) != node.max_rooms)
            throw Failure(409, "instance_configuration_changed");
    }
    else
    {
        if (found && existing.Number(3) > now) throw Failure(409, "node_already_live");
        Query retired(db_, "SELECT 1 FROM cluster_instances WHERE node=? AND instance=?");
        retired.Text(1, node.id).Text(2, node.instance);
        if (retired.Step()) throw Failure(409, "instance_retired");
        Query remember(db_, "INSERT INTO cluster_instances(node,instance) VALUES(?,?)");
        remember.Text(1, node.id).Text(2, node.instance).Step();
    }
    Query update(db_, "INSERT INTO cluster_nodes(id,instance,url,capacity,expires,participants) VALUES(?,?,?,?,?,?) "
        "ON CONFLICT(id) DO UPDATE SET instance=excluded.instance,url=excluded.url,capacity=excluded.capacity,"
        "expires=excluded.expires,participants=excluded.participants");
    update.Text(1, node.id).Text(2, node.instance).Text(3, node.signaling_url).Number(4, node.max_rooms)
        .Number(5, now + kLeaseSeconds).Number(6, node.participants).Step();
    transaction.Commit();
}
void Coordinator::Heartbeat(const std::string& node, const std::string& instance, int participants)
{
    RequireId(node); RequireId(instance);
    if (participants < 0 || participants > 65536) throw Failure(400, "invalid_load");
    std::lock_guard<std::mutex> lock(mutex_);
    Transaction transaction(db_);
    Query update(db_, "UPDATE cluster_nodes SET expires=?,participants=? WHERE id=? AND instance=?");
    update.Number(1, clock_() + kLeaseSeconds).Number(2, participants).Text(3, node).Text(4, instance).Step();
    if (sqlite3_changes(db_) != 1) throw Failure(409, "instance_fenced");
    transaction.Commit();
}
std::string Coordinator::CreateRoom(const std::string& owner)
{
    RequireId(owner);
    std::lock_guard<std::mutex> lock(mutex_);
    Transaction transaction(db_);
    Query count(db_, "SELECT COUNT(*) FROM cluster_rooms WHERE owner=?");
    count.Text(1, owner); count.Step();
    if (count.Number(0) >= 100) throw Failure(409, "room_quota_exceeded");
    const auto room = RandomId();
    Query create(db_, "INSERT INTO cluster_rooms(id,owner) VALUES(?,?)");
    create.Text(1, room).Text(2, owner).Step();
    Query member(db_, "INSERT INTO cluster_members(room,user,publish) VALUES(?,?,1)");
    member.Text(1, room).Text(2, owner).Step();
    transaction.Commit();
    return room;
}
void Coordinator::AddMember(const std::string& room, const std::string& owner,
                            const std::string& user, bool can_publish)
{
    RequireId(room); RequireId(owner); RequireId(user);
    std::lock_guard<std::mutex> lock(mutex_);
    Transaction transaction(db_);
    Query query(db_, "SELECT owner FROM cluster_rooms WHERE id=?");
    query.Text(1, room);
    if (!query.Step() || query.Text(0) != owner) throw Failure(403, "room_owner_required");
    if (user == owner && !can_publish) throw Failure(400, "owner_role_required");
    Query count(db_, "SELECT COUNT(*) FROM cluster_members WHERE room=? AND user<>?");
    count.Text(1, room).Text(2, user); count.Step();
    if (count.Number(0) >= 1000) throw Failure(409, "member_quota_exceeded");
    Query insert(db_, "INSERT INTO cluster_members(room,user,publish) VALUES(?,?,?) "
        "ON CONFLICT(room,user) DO UPDATE SET publish=excluded.publish");
    insert.Text(1, room).Text(2, user).Number(3, can_publish).Step();
    transaction.Commit();
}
Admission Coordinator::Admit(const std::string& room, const std::string& user)
{
    RequireId(room); RequireId(user);
    std::lock_guard<std::mutex> lock(mutex_);
    Transaction transaction(db_);
    Query membership(db_, "SELECT publish FROM cluster_members WHERE room=? AND user=?");
    membership.Text(1, room).Text(2, user);
    if (!membership.Step()) throw Failure(403, "room_membership_required");
    Admission admission;
    admission.can_publish = membership.Number(0) != 0;
    Query binding(db_, "SELECT node,instance FROM cluster_rooms WHERE id=?");
    binding.Text(1, room); binding.Step();
    const auto node = binding.Text(0), instance = binding.Text(1);
    const auto now = clock_();
    if (!node.empty())
    {
        Query query(db_, "SELECT id,instance,url,capacity,participants FROM cluster_nodes WHERE id=? AND instance=? AND expires>?");
        query.Text(1, node).Text(2, instance).Number(3, now);
        if (!query.Step()) throw Failure(503, "room_node_unavailable");
        admission.node = {query.Text(0), query.Text(1), query.Text(2), static_cast<int>(query.Number(3)), static_cast<int>(query.Number(4))};
    }
    else
    {
        Query query(db_, "SELECT n.id,n.instance,n.url,n.capacity,n.participants,COUNT(r.id) AS assigned "
            "FROM cluster_nodes n LEFT JOIN cluster_rooms r ON r.node=n.id AND r.instance=n.instance "
            "WHERE n.expires>? GROUP BY n.id HAVING assigned<n.capacity "
            "ORDER BY CAST(assigned AS REAL)/n.capacity,n.participants,n.id LIMIT 1");
        query.Number(1, now);
        if (!query.Step()) throw Failure(503, "no_available_node");
        admission.node = {query.Text(0), query.Text(1), query.Text(2), static_cast<int>(query.Number(3)), static_cast<int>(query.Number(4))};
        Query assign(db_, "UPDATE cluster_rooms SET node=?,instance=? WHERE id=? AND node IS NULL");
        assign.Text(1, admission.node.id).Text(2, admission.node.instance).Text(3, room).Step();
        if (sqlite3_changes(db_) != 1) throw Failure(503, "assignment_conflict");
    }
    transaction.Commit();
    return admission;
}
}
