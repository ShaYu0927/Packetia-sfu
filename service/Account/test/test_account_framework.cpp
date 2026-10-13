#include "service/Account/AccountService.h"
#include "service/Account/ScryptPasswordHasher.h"
#include "service/Account/SqliteAccountStore.h"

#include <gtest/gtest.h>
#include <stdexcept>

namespace service::account
{
namespace
{

class LifecycleStore final : public AccountStore
{
public:
    int open_calls = 0;
    int close_calls = 0;
    AccountError open_error = AccountError::None;
    bool throw_on_open = false;

    AccountStatus Open(std::string_view) override
    {
        ++open_calls;
        if (throw_on_open)
            throw std::runtime_error("storage initialization failed");
        return open_error == AccountError::None ? AccountStatus::Success()
                                                : AccountStatus::Failure(open_error);
    }
    void Close() noexcept override { ++close_calls; }
    AccountStatus InsertUser(const StoredUser&) override
    { return AccountStatus::Failure(AccountError::NotImplemented); }
    AccountResult<StoredUser> FindUserByLogin(std::string_view) override
    { return AccountResult<StoredUser>::Failure(AccountError::NotImplemented); }
    AccountResult<StoredSession> FindSessionByDigest(std::string_view) override
    { return AccountResult<StoredSession>::Failure(AccountError::NotImplemented); }
    AccountStatus InsertSessionIfCurrent(const LoginSession&, int64_t) override
    { return AccountStatus::Failure(AccountError::NotImplemented); }
    AccountStatus RotateSessionIfCurrent(std::string_view, const LoginSession&, int64_t) override
    { return AccountStatus::Failure(AccountError::NotImplemented); }
    AccountStatus RevokeSession(std::string_view, int64_t) override
    { return AccountStatus::Failure(AccountError::NotImplemented); }
};

class ReadyHasher final : public PasswordHasher
{
public:
    bool IsReady() const noexcept override { return true; }
    AccountResult<std::string> Hash(std::string_view) const override
    { return AccountResult<std::string>::Failure(AccountError::NotImplemented); }
    AccountStatus Verify(std::string_view, std::string_view) const override
    { return AccountStatus::Failure(AccountError::NotImplemented); }
};

AccountOptions EnabledOptions()
{
    AccountOptions options;
    options.enabled = true;
    return options;
}

TEST(AccountFramework, DisabledAndMissingDependenciesNeverOpenStorage)
{
    const auto store = std::make_shared<LifecycleStore>();
    AccountService disabled({}, store, std::make_shared<ReadyHasher>());
    EXPECT_EQ(disabled.Start().error, AccountError::NotConfigured);
    EXPECT_EQ(disabled.ResolveSession("anything").error, AccountError::NotConfigured);
    EXPECT_EQ(store->open_calls, 0);

    AccountService missing_hasher(EnabledOptions(), store, nullptr);
    EXPECT_EQ(missing_hasher.Start().error, AccountError::NotConfigured);
    EXPECT_EQ(store->open_calls, 0);
}

TEST(AccountFramework, PlaceholderAdaptersCannotEnableAuthentication)
{
    const auto store = std::make_shared<LifecycleStore>();
    const auto hasher = std::make_shared<ScryptPasswordHasher>();
    AccountService service(EnabledOptions(), store, hasher);
    EXPECT_EQ(service.Start().error, AccountError::NotImplemented);
    EXPECT_EQ(store->open_calls, 0);
    EXPECT_FALSE(hasher->Verify("password", "password").Succeeded());
    EXPECT_FALSE(service.Login({"alice", "password"}).Succeeded());
    EXPECT_FALSE(service.ResolveSession("token").Succeeded());

    AccountService sqlite_service(EnabledOptions(), std::make_shared<SqliteAccountStore>(),
                                  std::make_shared<ReadyHasher>());
    EXPECT_EQ(sqlite_service.Start().error, AccountError::NotImplemented);
    EXPECT_FALSE(sqlite_service.ResolveSession("token").Succeeded());
}

TEST(AccountFramework, FailedStorageStartupCleansUpAndCanBeRetried)
{
    const auto store = std::make_shared<LifecycleStore>();
    store->open_error = AccountError::StorageError;
    AccountService service(EnabledOptions(), store, std::make_shared<ReadyHasher>());
    EXPECT_EQ(service.Start().error, AccountError::StorageError);
    EXPECT_EQ(store->close_calls, 1);
    EXPECT_EQ(service.Login({"alice", "password"}).error, AccountError::Unavailable);
    store->open_error = AccountError::None;
    EXPECT_TRUE(service.Start().Succeeded());
    EXPECT_TRUE(service.Start().Succeeded());
    EXPECT_EQ(store->open_calls, 2);
    service.Stop();
    service.Stop();
    EXPECT_EQ(store->close_calls, 2);
    EXPECT_EQ(service.ResolveSession("token").error, AccountError::Unavailable);
}

TEST(AccountFramework, ReadyDependenciesStillCannotIssueFakeAccountsOrCredentials)
{
    const auto store = std::make_shared<LifecycleStore>();
    {
        AccountService service(EnabledOptions(), store, std::make_shared<ReadyHasher>());
        ASSERT_TRUE(service.Start().Succeeded());
        const auto created = service.CreateAccount({"alice", "password", "Alice"});
        EXPECT_EQ(created.error, AccountError::NotImplemented);
        EXPECT_FALSE(created.value.has_value());
        const auto login = service.Login({"alice", "password"});
        EXPECT_EQ(login.error, AccountError::NotImplemented);
        EXPECT_FALSE(login.value.has_value());
        EXPECT_FALSE(service.ResolveSession("token").Succeeded());
        EXPECT_FALSE(service.RenewSession("token").Succeeded());
        EXPECT_FALSE(service.Logout("token").Succeeded());
    }
    EXPECT_EQ(store->close_calls, 1);
}

TEST(AccountFramework, ThrowingStorageStartupCleansUpAndLeavesServiceUnavailable)
{
    const auto store = std::make_shared<LifecycleStore>();
    store->throw_on_open = true;
    AccountService service(EnabledOptions(), store, std::make_shared<ReadyHasher>());
    EXPECT_EQ(service.Start().error, AccountError::StorageError);
    EXPECT_EQ(store->close_calls, 1);
    EXPECT_EQ(service.ResolveSession("token").error, AccountError::Unavailable);
    store->throw_on_open = false;
    EXPECT_TRUE(service.Start().Succeeded());
    service.Stop();
    EXPECT_EQ(store->close_calls, 2);
}

TEST(AccountFramework, InvalidWorkerConfigurationIsRejectedBeforeStorageOpen)
{
    auto options = EnabledOptions();
    options.worker_count = 65;
    const auto store = std::make_shared<LifecycleStore>();
    AccountService service(options, store, std::make_shared<ReadyHasher>());
    EXPECT_EQ(service.Start().error, AccountError::InvalidArgument);
    EXPECT_EQ(store->open_calls, 0);
}

}
}
