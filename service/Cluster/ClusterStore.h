#pragma once
#include <sqlite3.h>
#include <mutex>
#include <string>
#include "ClusterTypes.h"

namespace service::cluster
{
namespace storage
{
inline void Check(int result)
{
    if (result != SQLITE_OK && result != SQLITE_DONE && result != SQLITE_ROW)
        throw Failure(503, "storage_unavailable");
}
inline void Exec(sqlite3* db, const char* sql) { Check(sqlite3_exec(db, sql, nullptr, nullptr, nullptr)); }
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
}
// 会议记录与节点绑定共享数据库事务，但业务操作和节点调度由不同服务负责。
class ClusterStore final
{
public:
    explicit ClusterStore(const std::string& path);
    ~ClusterStore();
    ClusterStore(const ClusterStore&) = delete;
    ClusterStore& operator=(const ClusterStore&) = delete;
    class Write
    {
    public:
        explicit Write(ClusterStore& store) : lock_(store.mutex_), db_(store.db_), transaction_(db_) {}
        storage::Query Prepare(const char* sql) { return storage::Query(db_, sql); }
        int Changes() const { return sqlite3_changes(db_); }
        void Commit() { transaction_.Commit(); }
    private:
        std::unique_lock<std::mutex> lock_;
        sqlite3* db_;
        storage::Transaction transaction_;
    };
private:
    sqlite3* db_ = nullptr;
    std::mutex mutex_;
};
}
