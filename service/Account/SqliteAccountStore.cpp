#include "SqliteAccountStore.h"

namespace service::account
{

AccountStatus SqliteAccountStore::Open(std::string_view)
{
    // TODO: Open SQLite and apply transactional schema migrations.
    return AccountStatus::Failure(AccountError::NotImplemented);
}

void SqliteAccountStore::Close() noexcept
{
}

AccountStatus SqliteAccountStore::InsertUser(const StoredUser&)
{
    return AccountStatus::Failure(AccountError::NotImplemented);
}

AccountResult<StoredUser> SqliteAccountStore::FindUserByLogin(std::string_view)
{
    return AccountResult<StoredUser>::Failure(AccountError::NotImplemented);
}

AccountResult<StoredSession> SqliteAccountStore::FindSessionByDigest(std::string_view)
{
    return AccountResult<StoredSession>::Failure(AccountError::NotImplemented);
}

AccountStatus SqliteAccountStore::InsertSessionIfCurrent(const LoginSession&, int64_t)
{
    return AccountStatus::Failure(AccountError::NotImplemented);
}

AccountStatus SqliteAccountStore::RotateSessionIfCurrent(std::string_view,
                                                        const LoginSession&, int64_t)
{
    return AccountStatus::Failure(AccountError::NotImplemented);
}

AccountStatus SqliteAccountStore::RevokeSession(std::string_view, int64_t)
{
    return AccountStatus::Failure(AccountError::NotImplemented);
}

}
