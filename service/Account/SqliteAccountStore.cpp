#include "SqliteAccountStore.h"
#include "AccountPolicy.h"

#include <sqlite3.h>
#include <filesystem>
#include <limits>
#include <memory>
#include <stdexcept>

namespace service::account
{
namespace
{
void Exec(sqlite3* db, const char* sql)
{
    if (sqlite3_exec(db, sql, nullptr, nullptr, nullptr) != SQLITE_OK)
        throw std::runtime_error("account database operation failed");
}

class Statement
{
public:
    Statement(sqlite3* db, const char* sql)
    {
        if (sqlite3_prepare_v2(db, sql, -1, &value_, nullptr) != SQLITE_OK)
            throw std::runtime_error("account statement preparation failed");
    }
    ~Statement() { sqlite3_finalize(value_); }
    Statement(const Statement&) = delete;
    Statement& operator=(const Statement&) = delete;
    void Bind(int index, std::string_view value)
    {
        if (value.size() > static_cast<size_t>(std::numeric_limits<int>::max()) ||
            sqlite3_bind_text(value_, index, value.empty() ? "" : value.data(),
                              static_cast<int>(value.size()), SQLITE_TRANSIENT) != SQLITE_OK)
            throw std::runtime_error("account text binding failed");
    }
    void Bind(int index, int64_t value)
    {
        if (sqlite3_bind_int64(value_, index, value) != SQLITE_OK)
            throw std::runtime_error("account integer binding failed");
    }
    int Step() { return sqlite3_step(value_); }
    bool Row()
    {
        const auto code = Step();
        if (code != SQLITE_ROW && code != SQLITE_DONE)
            throw std::runtime_error("account query failed");
        return code == SQLITE_ROW;
    }
    void Done()
    {
        if (Step() != SQLITE_DONE) throw std::runtime_error("account write failed");
    }
    std::string Text(int column) const
    {
        const auto* text = sqlite3_column_text(value_, column);
        return text ? std::string(reinterpret_cast<const char*>(text),
                                  static_cast<size_t>(sqlite3_column_bytes(value_, column))) : std::string{};
    }
    int64_t Integer(int column) const { return sqlite3_column_int64(value_, column); }
private:
    sqlite3_stmt* value_ = nullptr;
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

User ReadUser(const Statement& row, int start)
{
    return {row.Text(start), row.Text(start + 1), row.Text(start + 2),
            row.Integer(start + 3) == 0 ? UserStatus::Active : UserStatus::Disabled,
            static_cast<uint64_t>(row.Integer(start + 4)), row.Integer(start + 5)};
}

AccountResult<StoredSession> FindSession(sqlite3* db, std::string_view digest)
{
    Statement row(db, "SELECT s.session_id,s.user_id,s.token_digest,s.credential_version,"
        "s.created_at,s.expires_at,s.revoked_at,u.id,u.login,u.display_name,u.status,"
        "u.credential_version,u.created_at FROM account_sessions s "
        "JOIN account_users u ON u.id=s.user_id WHERE s.token_digest=?");
    row.Bind(1, digest);
    if (!row.Row()) return AccountResult<StoredSession>::Failure(AccountError::NotFound);
    LoginSession session{row.Text(0), row.Text(1), row.Text(2),
        static_cast<uint64_t>(row.Integer(3)), row.Integer(4), row.Integer(5), row.Integer(6)};
    return AccountResult<StoredSession>::Success({std::move(session), ReadUser(row, 7)});
}

bool ValidDigest(std::string_view digest)
{
    if (digest.size() != 64) return false;
    for (const auto ch : digest)
        if (!((ch >= '0' && ch <= '9') || (ch >= 'a' && ch <= 'f'))) return false;
    return true;
}

bool ValidNewSession(const LoginSession& session, int64_t now)
{
    return !session.session_id.empty() && session.session_id.size() <= 128 &&
           !session.user_id.empty() && session.user_id.size() <= 128 &&
           ValidDigest(session.token_digest) && session.credential_version > 0 &&
           session.credential_version <= static_cast<uint64_t>(std::numeric_limits<int64_t>::max()) &&
           now > 0 && session.created_at_unix_seconds == now &&
           session.expires_at_unix_seconds > now && session.revoked_at_unix_seconds == 0;
}

void InsertSession(sqlite3* db, const LoginSession& session)
{
    Statement insert(db, "INSERT INTO account_sessions "
        "(session_id,user_id,token_digest,credential_version,created_at,expires_at,revoked_at) "
        "VALUES(?,?,?,?,?,?,0)");
    insert.Bind(1, session.session_id);
    insert.Bind(2, session.user_id);
    insert.Bind(3, session.token_digest);
    insert.Bind(4, static_cast<int64_t>(session.credential_version));
    insert.Bind(5, session.created_at_unix_seconds);
    insert.Bind(6, session.expires_at_unix_seconds);
    insert.Done();
}
}

SqliteAccountStore::~SqliteAccountStore() { Close(); }

AccountStatus SqliteAccountStore::Open(std::string_view database_path)
{
    std::lock_guard<std::mutex> lock(mutex_);
    if (db_) return AccountStatus::Failure(AccountError::Unavailable);
    if (database_path.empty() || database_path.find('\0') != std::string_view::npos)
        return AccountStatus::Failure(AccountError::InvalidArgument);
    try
    {
        const std::string path(database_path);
        const auto parent = std::filesystem::path(path).parent_path();
        if (!parent.empty()) std::filesystem::create_directories(parent);
        sqlite3* raw = nullptr;
        const auto opened = sqlite3_open_v2(path.c_str(), &raw,
            SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE | SQLITE_OPEN_FULLMUTEX | SQLITE_OPEN_PRIVATECACHE, nullptr);
        std::unique_ptr<sqlite3, decltype(&sqlite3_close_v2)> handle(raw, sqlite3_close_v2);
        if (opened != SQLITE_OK || sqlite3_busy_timeout(raw, 5000) != SQLITE_OK ||
            sqlite3_extended_result_codes(raw, 1) != SQLITE_OK)
            return AccountStatus::Failure(AccountError::StorageError);
        Exec(raw, "PRAGMA foreign_keys=ON; PRAGMA journal_mode=WAL; PRAGMA synchronous=FULL;");
        Transaction transaction(raw);
        // Account schema version is namespaced: recording may share packetia.db
        // and owns its own PRAGMA user_version.
        Exec(raw, "CREATE TABLE IF NOT EXISTS account_schema_version ("
                  "id INTEGER PRIMARY KEY CHECK(id=1),version INTEGER NOT NULL)");
        int64_t version = 0;
        {
            Statement row(raw, "SELECT version FROM account_schema_version WHERE id=1");
            if (row.Row()) version = row.Integer(0);
        }
        if (version != 0 && version != 1) return AccountStatus::Failure(AccountError::StorageError);
        if (version == 0)
        {
            Exec(raw,
                "CREATE TABLE account_users ("
                "id TEXT PRIMARY KEY NOT NULL,login TEXT UNIQUE NOT NULL CHECK(login=lower(login)),"
                "display_name TEXT NOT NULL,status INTEGER NOT NULL CHECK(status IN (0,1)),"
                "credential_version INTEGER NOT NULL CHECK(credential_version>0),"
                "created_at INTEGER NOT NULL CHECK(created_at>0));"
                "CREATE TABLE account_password_credentials ("
                "user_id TEXT PRIMARY KEY NOT NULL REFERENCES account_users(id) ON DELETE CASCADE,"
                "password_hash TEXT NOT NULL);"
                "CREATE TABLE account_sessions ("
                "session_id TEXT PRIMARY KEY NOT NULL,"
                "user_id TEXT NOT NULL REFERENCES account_users(id) ON DELETE CASCADE,"
                "token_digest TEXT UNIQUE NOT NULL CHECK(length(token_digest)=64),"
                "credential_version INTEGER NOT NULL CHECK(credential_version>0),"
                "created_at INTEGER NOT NULL CHECK(created_at>0),"
                "expires_at INTEGER NOT NULL CHECK(expires_at>created_at),"
                "revoked_at INTEGER NOT NULL DEFAULT 0 CHECK(revoked_at>=0));"
                "CREATE INDEX account_sessions_user ON account_sessions(user_id);"
                "CREATE INDEX account_sessions_expiry ON account_sessions(expires_at);"
                "INSERT INTO account_schema_version(id,version) VALUES(1,1);");
        }
        transaction.Commit();
        db_ = handle.release();
        return AccountStatus::Success();
    }
    catch (...) { return AccountStatus::Failure(AccountError::StorageError); }
}

void SqliteAccountStore::Close() noexcept
{
    std::lock_guard<std::mutex> lock(mutex_);
    if (db_) sqlite3_close_v2(db_);
    db_ = nullptr;
}

AccountStatus SqliteAccountStore::InsertUser(const StoredUser& stored)
{
    std::lock_guard<std::mutex> lock(mutex_);
    if (!db_) return AccountStatus::Failure(AccountError::Unavailable);
    const auto& user = stored.user;
    std::string normalized;
    if (user.id.empty() || user.id.size() > 128 || !NormalizeLogin(user.login, normalized) ||
        normalized != user.login || !ValidDisplayName(user.display_name) ||
        user.credential_version == 0 ||
        user.credential_version > static_cast<uint64_t>(std::numeric_limits<int64_t>::max()) ||
        user.created_at_unix_seconds <= 0 || stored.password_hash.empty() || stored.password_hash.size() > 1024 ||
        (user.status != UserStatus::Active && user.status != UserStatus::Disabled))
        return AccountStatus::Failure(AccountError::InvalidArgument);
    try
    {
        Transaction transaction(db_);
        Statement insert(db_, "INSERT INTO account_users "
            "(id,login,display_name,status,credential_version,created_at) VALUES(?,?,?,?,?,?)");
        insert.Bind(1, user.id); insert.Bind(2, user.login); insert.Bind(3, user.display_name);
        insert.Bind(4, user.status == UserStatus::Active ? int64_t{0} : int64_t{1});
        insert.Bind(5, static_cast<int64_t>(user.credential_version));
        insert.Bind(6, user.created_at_unix_seconds);
        const auto code = insert.Step();
        if (code == SQLITE_CONSTRAINT_UNIQUE) return AccountStatus::Failure(AccountError::AlreadyExists);
        if (code != SQLITE_DONE) return AccountStatus::Failure(AccountError::StorageError);
        Statement credential(db_, "INSERT INTO account_password_credentials(user_id,password_hash) VALUES(?,?)");
        credential.Bind(1, user.id); credential.Bind(2, stored.password_hash); credential.Done();
        transaction.Commit();
        return AccountStatus::Success();
    }
    catch (...) { return AccountStatus::Failure(AccountError::StorageError); }
}

AccountResult<StoredUser> SqliteAccountStore::FindUserByLogin(std::string_view login)
{
    std::lock_guard<std::mutex> lock(mutex_);
    if (!db_) return AccountResult<StoredUser>::Failure(AccountError::Unavailable);
    try
    {
        Statement row(db_, "SELECT u.id,u.login,u.display_name,u.status,u.credential_version,u.created_at,"
            "p.password_hash FROM account_users u JOIN account_password_credentials p ON p.user_id=u.id "
            "WHERE u.login=?");
        row.Bind(1, login);
        if (!row.Row()) return AccountResult<StoredUser>::Failure(AccountError::NotFound);
        return AccountResult<StoredUser>::Success({ReadUser(row, 0), row.Text(6)});
    }
    catch (...) { return AccountResult<StoredUser>::Failure(AccountError::StorageError); }
}

AccountResult<StoredSession> SqliteAccountStore::FindSessionByDigest(std::string_view digest)
{
    std::lock_guard<std::mutex> lock(mutex_);
    if (!db_) return AccountResult<StoredSession>::Failure(AccountError::Unavailable);
    try { return FindSession(db_, digest); }
    catch (...) { return AccountResult<StoredSession>::Failure(AccountError::StorageError); }
}

AccountStatus SqliteAccountStore::InsertSessionIfCurrent(const LoginSession& session, int64_t now)
{
    std::lock_guard<std::mutex> lock(mutex_);
    if (!db_) return AccountStatus::Failure(AccountError::Unavailable);
    if (!ValidNewSession(session, now)) return AccountStatus::Failure(AccountError::InvalidArgument);
    try
    {
        Transaction transaction(db_);
        Statement user(db_, "SELECT status,credential_version FROM account_users WHERE id=?");
        user.Bind(1, session.user_id);
        if (!user.Row()) return AccountStatus::Failure(AccountError::InvalidCredentials);
        if (user.Integer(0) != 0) return AccountStatus::Failure(AccountError::UserDisabled);
        if (static_cast<uint64_t>(user.Integer(1)) != session.credential_version)
            return AccountStatus::Failure(AccountError::InvalidCredentials);
        InsertSession(db_, session);
        transaction.Commit();
        return AccountStatus::Success();
    }
    catch (...) { return AccountStatus::Failure(AccountError::StorageError); }
}

AccountStatus SqliteAccountStore::RotateSessionIfCurrent(std::string_view previous_digest,
                                                        const LoginSession& replacement, int64_t now)
{
    std::lock_guard<std::mutex> lock(mutex_);
    if (!db_) return AccountStatus::Failure(AccountError::Unavailable);
    if (!ValidNewSession(replacement, now) || !ValidDigest(previous_digest) ||
        previous_digest == replacement.token_digest)
        return AccountStatus::Failure(AccountError::InvalidArgument);
    try
    {
        Transaction transaction(db_);
        const auto previous = FindSession(db_, previous_digest);
        if (!previous.Succeeded()) return AccountStatus::Failure(previous.error);
        const auto validity = SessionValidity(*previous.value, now);
        if (validity != AccountError::None) return AccountStatus::Failure(validity);
        if (replacement.user_id != previous.value->user.id ||
            replacement.credential_version != previous.value->user.credential_version ||
            replacement.session_id == previous.value->session.session_id)
            return AccountStatus::Failure(AccountError::InvalidArgument);
        Statement revoke(db_, "UPDATE account_sessions SET revoked_at=? WHERE token_digest=? AND revoked_at=0");
        revoke.Bind(1, now); revoke.Bind(2, previous_digest); revoke.Done();
        if (sqlite3_changes(db_) != 1) return AccountStatus::Failure(AccountError::SessionRevoked);
        InsertSession(db_, replacement);
        transaction.Commit();
        return AccountStatus::Success();
    }
    catch (...) { return AccountStatus::Failure(AccountError::StorageError); }
}

AccountStatus SqliteAccountStore::RevokeSession(std::string_view digest, int64_t now)
{
    std::lock_guard<std::mutex> lock(mutex_);
    if (!db_) return AccountStatus::Failure(AccountError::Unavailable);
    if (!ValidDigest(digest) || now <= 0) return AccountStatus::Failure(AccountError::InvalidArgument);
    try
    {
        Statement revoke(db_, "UPDATE account_sessions SET revoked_at=? WHERE token_digest=? AND revoked_at=0");
        revoke.Bind(1, now); revoke.Bind(2, digest); revoke.Done();
        return AccountStatus::Success();
    }
    catch (...) { return AccountStatus::Failure(AccountError::StorageError); }
}

}
