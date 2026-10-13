#ifndef PACKETIA_SERVICE_ACCOUNT_SQLITE_ACCOUNT_STORE_H_
#define PACKETIA_SERVICE_ACCOUNT_SQLITE_ACCOUNT_STORE_H_

#include "AccountStore.h"

namespace service::account
{

// Framework placeholder. Open fails explicitly and does not create a database.
class SqliteAccountStore final : public AccountStore
{
public:
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
};

}

#endif // PACKETIA_SERVICE_ACCOUNT_SQLITE_ACCOUNT_STORE_H_
