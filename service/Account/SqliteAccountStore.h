#ifndef PACKETIA_SERVICE_ACCOUNT_SQLITE_ACCOUNT_STORE_H_
#define PACKETIA_SERVICE_ACCOUNT_SQLITE_ACCOUNT_STORE_H_

#include "AccountStore.h"
#include <mutex>

struct sqlite3;

namespace service::account
{

// One synchronized connection per store; write transactions also coordinate
// independent store instances/processes using the same database.
class SqliteAccountStore final : public AccountStore
{
public:
    SqliteAccountStore() = default;
    ~SqliteAccountStore() override;
    SqliteAccountStore(const SqliteAccountStore&) = delete;
    SqliteAccountStore& operator=(const SqliteAccountStore&) = delete;
    AccountStatus Open(std::string_view database_path) override;
    void Close() noexcept override;
    AccountStatus InsertUser(const StoredUser& user) override;
    AccountResult<StoredUser> FindUserByLogin(std::string_view login) override;
    AccountResult<StoredSession> FindSessionByDigest(std::string_view digest) override;
    AccountStatus InsertSessionIfCurrent(const LoginSession& session,
                                         int64_t now_unix_seconds) override;
    AccountStatus RotateSessionIfCurrent(std::string_view previous_digest,
                                         const LoginSession& replacement,
                                         int64_t now_unix_seconds) override;
    AccountStatus RevokeSession(std::string_view digest,
                                int64_t now_unix_seconds) override;
private:
    std::mutex mutex_;
    sqlite3* db_ = nullptr;
};

}

#endif // PACKETIA_SERVICE_ACCOUNT_SQLITE_ACCOUNT_STORE_H_
