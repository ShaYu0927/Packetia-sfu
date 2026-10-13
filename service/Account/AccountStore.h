#ifndef PACKETIA_SERVICE_ACCOUNT_STORE_H_
#define PACKETIA_SERVICE_ACCOUNT_STORE_H_

#include "AccountTypes.h"

#include <string_view>

namespace service::account
{

// Persistence boundary. Implementations synchronize concurrent business workers.
// AccountService owns Open/Close; callers must not close a live service's store.
class AccountStore
{
public:
    virtual ~AccountStore() = default;

    virtual AccountStatus Open(std::string_view database_path) = 0;
    virtual void Close() noexcept = 0;

    // Enforce normalized login uniqueness atomically; return AlreadyExists.
    virtual AccountStatus InsertUser(const StoredUser& user) = 0;
    virtual AccountResult<StoredUser> FindUserByLogin(std::string_view login) = 0;

    // Return NotFound when absent. Both records come from one consistent snapshot.
    virtual AccountResult<StoredSession> FindSessionByDigest(std::string_view digest) = 0;

    // In one transaction, require an active user and matching credential_version.
    // This prevents a password change/disable racing password verification.
    virtual AccountStatus InsertSessionIfCurrent(const LoginSession& session,
                                                 int64_t now_unix_seconds) = 0;

    // Atomically validate the previous session/user, revoke the previous token,
    // and insert the replacement for the same user. Concurrent renewal may win
    // only once; require unexpired/unrevoked session and current credentials.
    virtual AccountStatus RotateSessionIfCurrent(std::string_view previous_digest,
                                                 const LoginSession& replacement,
                                                 int64_t now_unix_seconds) = 0;

    // Idempotent logout: an absent or already revoked session is also success.
    virtual AccountStatus RevokeSession(std::string_view digest,
                                        int64_t now_unix_seconds) = 0;
};

}

#endif // PACKETIA_SERVICE_ACCOUNT_STORE_H_
