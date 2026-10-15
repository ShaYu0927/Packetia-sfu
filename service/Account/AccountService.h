#ifndef PACKETIA_SERVICE_ACCOUNT_SERVICE_H_
#define PACKETIA_SERVICE_ACCOUNT_SERVICE_H_

#include "AccountOptions.h"
#include "AccountStore.h"
#include "PasswordHasher.h"

#include <memory>
#include <functional>
#include <shared_mutex>
#include <string_view>

namespace service::account
{

class AccountService
{
public:
    using Clock = std::function<int64_t()>; // Unix seconds; safe for concurrent callers.
    AccountService(AccountOptions options, std::shared_ptr<AccountStore> store,
                   std::shared_ptr<PasswordHasher> password_hasher, Clock clock = {});
    ~AccountService();

    AccountStatus Start();
    void Stop() noexcept;

    AccountResult<User> CreateAccount(const CreateAccountRequest& request);
    AccountResult<LoginResult> Login(const LoginRequest& request);
    AccountResult<AccountIdentity> ResolveSession(std::string_view token);
    AccountResult<LoginResult> RenewSession(std::string_view token);
    AccountStatus Logout(std::string_view token);

private:
    AccountError CheckReadyLocked() const;

    const AccountOptions options_;
    const std::shared_ptr<AccountStore> store_;
    const std::shared_ptr<PasswordHasher> password_hasher_;
    const Clock clock_;
    std::string dummy_password_hash_;
    std::shared_mutex mutex_;
    bool started_ = false;
};

}

#endif // PACKETIA_SERVICE_ACCOUNT_SERVICE_H_
