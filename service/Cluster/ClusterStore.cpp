#include "ClusterStore.h"
#include <set>
namespace service::cluster
{
using namespace storage;
ClusterStore::ClusterStore(const std::string& path)
{
    try
    {
        Check(sqlite3_open_v2(path.c_str(), &db_, SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE | SQLITE_OPEN_FULLMUTEX, nullptr));
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
        bool management = false;
        { Query info(db_, "PRAGMA table_info(cluster_nodes)");
          while (info.Step()) if (info.Text(1) == "management_url") management = true; }
        if (!management) Exec(db_, "ALTER TABLE cluster_nodes ADD COLUMN management_url TEXT NOT NULL DEFAULT ''");
        std::set<std::string> columns;
        Query info(db_, "PRAGMA table_info(cluster_rooms)");
        while (info.Step()) columns.insert(info.Text(1));
        if (!columns.count("state")) Exec(db_, "ALTER TABLE cluster_rooms ADD COLUMN state TEXT NOT NULL DEFAULT 'open'");
        if (!columns.count("assignment_id")) Exec(db_, "ALTER TABLE cluster_rooms ADD COLUMN assignment_id TEXT NOT NULL DEFAULT ''");
        if (!columns.count("reclaim_after")) Exec(db_, "ALTER TABLE cluster_rooms ADD COLUMN reclaim_after INTEGER NOT NULL DEFAULT 0");
        if (!columns.count("last_probe")) Exec(db_, "ALTER TABLE cluster_rooms ADD COLUMN last_probe INTEGER NOT NULL DEFAULT 0");
        Exec(db_, "UPDATE cluster_rooms SET assignment_id=lower(hex(randomblob(16))) WHERE node IS NOT NULL AND assignment_id=''");
    }
    catch (...) { if (db_) sqlite3_close(db_); db_ = nullptr; throw; }
}
ClusterStore::~ClusterStore() { if (db_) sqlite3_close(db_); }
}
