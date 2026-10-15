#ifndef PACKETIA_SERVICE_ACCOUNT_STORE_H_
#define PACKETIA_SERVICE_ACCOUNT_STORE_H_

#include "AccountTypes.h"

#include <string_view>

namespace service::account
{

class AccountStore
{
public:
    virtual ~AccountStore() = default;

    virtual AccountStatus Open(std::string_view database_path) = 0;
    virtual void Close() noexcept = 0;

    virtual AccountStatus InsertUser(const StoredUser& user) = 0;
    virtual AccountResult<StoredUser> FindUserByLogin(std::string_view login) = 0;

    virtual AccountResult<StoredSession> FindSessionByDigest(std::string_view digest) = 0;

    virtual AccountStatus InsertSessionIfCurrent(const LoginSession& session, int64_t now_unix_seconds) = 0;

    virtual AccountStatus RotateSessionIfCurrent(std::string_view previous_digest, const LoginSession& replacement, int64_t now_unix_seconds) = 0;

    virtual AccountStatus RevokeSession(std::string_view digest, int64_t now_unix_seconds) = 0;
};

}

#endif // PACKETIA_SERVICE_ACCOUNT_STORE_H_
