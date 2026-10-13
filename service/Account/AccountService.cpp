#include "AccountService.h"

#include <mutex>
#include <utility>

namespace service::account
{

AccountService::AccountService(AccountOptions options, std::shared_ptr<AccountStore> store,
                               std::shared_ptr<PasswordHasher> password_hasher)
    : options_(std::move(options)), store_(std::move(store)),
      password_hasher_(std::move(password_hasher))
{
}

AccountService::~AccountService()
{
    Stop();
}

AccountStatus AccountService::Start()
{
    std::unique_lock<std::shared_mutex> lock(mutex_);
    if (!options_.enabled || !store_ || !password_hasher_)
        return AccountStatus::Failure(AccountError::NotConfigured);
    if (!options_.IsValid())
        return AccountStatus::Failure(AccountError::InvalidArgument);
    if (started_)
        return AccountStatus::Success();
    if (!password_hasher_->IsReady())
        return AccountStatus::Failure(AccountError::NotImplemented);

    try
    {
        const auto result = store_->Open(options_.database_path);
        if (!result.Succeeded())
        {
            store_->Close(); // Release partial initialization on a failed Open.
            return result;
        }
    }
    catch (...)
    {
        store_->Close();
        return AccountStatus::Failure(AccountError::StorageError);
    }
    started_ = true;
    return AccountStatus::Success();
}

void AccountService::Stop() noexcept
{
    std::unique_lock<std::shared_mutex> lock(mutex_);
    if (!started_)
        return;
    started_ = false;
    store_->Close();
}

AccountError AccountService::CheckReadyLocked() const
{
    if (!options_.enabled || !store_ || !password_hasher_)
        return AccountError::NotConfigured;
    return started_ ? AccountError::None : AccountError::Unavailable;
}

AccountResult<User> AccountService::CreateAccount(const CreateAccountRequest&)
{
    std::shared_lock<std::shared_mutex> lock(mutex_);
    const auto error = CheckReadyLocked();
    if (error != AccountError::None)
        return AccountResult<User>::Failure(error);
    // TODO: Validate registration policy, normalize login, hash password and insert.
    return AccountResult<User>::Failure(AccountError::NotImplemented);
}

AccountResult<LoginResult> AccountService::Login(const LoginRequest&)
{
    std::shared_lock<std::shared_mutex> lock(mutex_);
    const auto error = CheckReadyLocked();
    if (error != AccountError::None)
        return AccountResult<LoginResult>::Failure(error);
    // TODO: Verify password, generate a random credential and atomically store its
    // digest after rechecking active user/credential version. Never persist token.
    return AccountResult<LoginResult>::Failure(AccountError::NotImplemented);
}

AccountResult<AccountIdentity> AccountService::ResolveSession(std::string_view)
{
    std::shared_lock<std::shared_mutex> lock(mutex_);
    const auto error = CheckReadyLocked();
    if (error != AccountError::None)
        return AccountResult<AccountIdentity>::Failure(error);
    // TODO: Digest the credential, load a consistent session/user snapshot, and
    // check expiry, revocation, user status and credential version.
    return AccountResult<AccountIdentity>::Failure(AccountError::NotImplemented);
}

AccountResult<LoginResult> AccountService::RenewSession(std::string_view)
{
    std::shared_lock<std::shared_mutex> lock(mutex_);
    const auto error = CheckReadyLocked();
    if (error != AccountError::None)
        return AccountResult<LoginResult>::Failure(error);
    // TODO: Validate and rotate in one transaction so the old credential is spent.
    return AccountResult<LoginResult>::Failure(AccountError::NotImplemented);
}

AccountStatus AccountService::Logout(std::string_view)
{
    std::shared_lock<std::shared_mutex> lock(mutex_);
    const auto error = CheckReadyLocked();
    if (error != AccountError::None)
        return AccountStatus::Failure(error);
    // TODO: Persist revocation; clearing a client cookie alone is insufficient.
    return AccountStatus::Failure(AccountError::NotImplemented);
}

}
