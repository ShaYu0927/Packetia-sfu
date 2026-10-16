#include "AccountService.h"
#include "ScryptPasswordHasher.h"
#include "SqliteAccountStore.h"

#include <gtest/gtest.h>
#include <sqlite3.h>
#include <atomic>
#include <chrono>
#include <filesystem>
#include <future>
#include <functional>
#include <vector>

namespace service::account
{
namespace
{
constexpr const char* kPassword = "correct horse battery staple";
constexpr int64_t kNow = 1800000000;

class Database
{
public:
    Database()
    {
        static std::atomic<unsigned> serial{0};
        directory = std::filesystem::temp_directory_path() / ("packetia-accounts-" +
            std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()) + "-" +
            std::to_string(serial++));
        std::filesystem::create_directories(directory);
        path = (directory / "accounts.db").string();
    }
    ~Database() { std::error_code ignored; std::filesystem::remove_all(directory, ignored); }
    void Exec(const char* sql) const
    {
        sqlite3* db = nullptr;
        ASSERT_EQ(sqlite3_open(path.c_str(), &db), SQLITE_OK);
        sqlite3_busy_timeout(db, 5000);
        const auto result = sqlite3_exec(db, sql, nullptr, nullptr, nullptr);
        const std::string error = sqlite3_errmsg(db);
        sqlite3_close(db);
        ASSERT_EQ(result, SQLITE_OK) << error;
    }
    std::string Scalar(const char* sql) const
    {
        sqlite3* db = nullptr;
        if (sqlite3_open(path.c_str(), &db) != SQLITE_OK) { sqlite3_close(db); return {}; }
        sqlite3_stmt* row = nullptr;
        std::string result;
        if (sqlite3_prepare_v2(db, sql, -1, &row, nullptr) == SQLITE_OK && sqlite3_step(row) == SQLITE_ROW)
        {
            const auto* value = sqlite3_column_text(row, 0);
            if (value) result = reinterpret_cast<const char*>(value);
        }
        sqlite3_finalize(row);
        sqlite3_close(db);
        return result;
    }
    std::filesystem::path directory;
    std::string path;
};

AccountOptions Options(std::string path = ":memory:")
{
    AccountOptions options;
    options.enabled = options.registration_enabled = true;
    options.database_path = std::move(path);
    options.session_ttl_seconds = 60;
    return options;
}

class AccountBusiness : public testing::Test
{
protected:
    void SetUp() override
    {
        store = std::make_shared<SqliteAccountStore>();
        accounts = std::make_unique<AccountService>(Options(), store,
            std::make_shared<ScryptPasswordHasher>(), [this] { return now.load(); });
        ASSERT_TRUE(accounts->Start().Succeeded());
    }
    std::atomic<int64_t> now{kNow};
    std::shared_ptr<SqliteAccountStore> store;
    std::unique_ptr<AccountService> accounts;
};

TEST(ScryptPasswordHasher, MatchesIndependentFixtureAndRejectsWrongPassword)
{
    // hashlib.scrypt(password, salt=bytes(range(16)), n=32768, r=8, p=1, dklen=32).
    const std::string fixture = "$scrypt$ln=15,r=8,p=1$000102030405060708090a0b0c0d0e0f$"
        "7a8e34241db898d59175c696538c417467a975ffe569068425f16188d3159c58";
    ScryptPasswordHasher hasher;
    ASSERT_TRUE(hasher.IsReady());
    EXPECT_TRUE(hasher.Verify(kPassword, fixture).Succeeded());
    EXPECT_EQ(hasher.Verify("incorrect password", fixture).error, AccountError::InvalidCredentials);
    const auto first = hasher.Hash(kPassword), second = hasher.Hash(kPassword);
    ASSERT_TRUE(first.Succeeded()); ASSERT_TRUE(second.Succeeded());
    EXPECT_NE(*first.value, *second.value);
    EXPECT_TRUE(hasher.Verify(kPassword, *first.value).Succeeded());
    for (const auto& text : {std::string{}, std::string(kPassword), fixture + "$extra",
         std::string("$scrypt$ln=99,r=8,p=1$") + fixture.substr(21),
         std::string("$scrypt$ln=15,r=9,p=1$") + fixture.substr(21)})
        EXPECT_EQ(hasher.Verify(kPassword, text).error, AccountError::StorageError);
    EXPECT_EQ(hasher.Hash("").error, AccountError::InvalidArgument);
    EXPECT_EQ(hasher.Hash(std::string(4097, 'a')).error, AccountError::InvalidArgument);
    EXPECT_EQ(hasher.Hash(std::string("pw\0hidden", 9)).error, AccountError::InvalidArgument);
}

TEST_F(AccountBusiness, RegistersNormalizedIdentityAndRejectsDuplicateAndInvalidInput)
{
    const auto created = accounts->CreateAccount({"  Alice@Example.COM  ", kPassword, "小明"});
    ASSERT_TRUE(created.Succeeded());
    EXPECT_EQ(created.value->login, "alice@example.com");
    EXPECT_EQ(created.value->display_name, "小明");
    const auto stored = store->FindUserByLogin("alice@example.com");
    ASSERT_TRUE(stored.Succeeded());
    EXPECT_NE(stored.value->password_hash, kPassword);
    EXPECT_EQ(accounts->CreateAccount({"ALICE@example.com", kPassword, ""}).error, AccountError::AlreadyExists);
    for (const auto& login : {"ab", "has space", "user' OR 1=1 --", "用户名"})
        EXPECT_EQ(accounts->CreateAccount({login, kPassword, ""}).error, AccountError::InvalidArgument);
    EXPECT_EQ(accounts->CreateAccount({"bob", "short", ""}).error, AccountError::InvalidArgument);
    EXPECT_EQ(accounts->CreateAccount({"bob", kPassword, "bad\nname"}).error, AccountError::InvalidArgument);
    EXPECT_EQ(accounts->CreateAccount({"bob", kPassword, std::string("\xff", 1)}).error, AccountError::InvalidArgument);
    EXPECT_EQ(accounts->CreateAccount({"bob", kPassword, std::string(257, 'a')}).error, AccountError::InvalidArgument);
}

TEST_F(AccountBusiness, LoginResolvesRotatesAndRevokesOnlyItsOwnSession)
{
    ASSERT_TRUE(accounts->CreateAccount({"alice", kPassword, "Alice"}).Succeeded());
    EXPECT_EQ(accounts->Login({"alice", "incorrect password"}).error, AccountError::InvalidCredentials);
    EXPECT_EQ(accounts->Login({"missing", kPassword}).error, AccountError::InvalidCredentials);
    const auto first = accounts->Login({" ALICE ", kPassword});
    const auto second = accounts->Login({"alice", kPassword});
    ASSERT_TRUE(first.Succeeded()); ASSERT_TRUE(second.Succeeded());
    EXPECT_NE(first.value->session_token, second.value->session_token);
    auto resolved = accounts->ResolveSession(first.value->session_token);
    ASSERT_TRUE(resolved.Succeeded());
    EXPECT_EQ(resolved.value->user_id, first.value->identity.user_id);
    now += 10;
    const auto renewed = accounts->RenewSession(first.value->session_token);
    ASSERT_TRUE(renewed.Succeeded());
    EXPECT_EQ(renewed.value->expires_at_unix_seconds, kNow + 70);
    EXPECT_NE(renewed.value->session_token, first.value->session_token);
    EXPECT_EQ(accounts->ResolveSession(first.value->session_token).error, AccountError::SessionRevoked);
    EXPECT_EQ(accounts->RenewSession(first.value->session_token).error, AccountError::SessionRevoked);
    EXPECT_TRUE(accounts->Logout(renewed.value->session_token).Succeeded());
    EXPECT_TRUE(accounts->Logout(renewed.value->session_token).Succeeded());
    EXPECT_EQ(accounts->ResolveSession(renewed.value->session_token).error, AccountError::SessionRevoked);
    EXPECT_TRUE(accounts->ResolveSession(second.value->session_token).Succeeded());
    EXPECT_EQ(accounts->ResolveSession("bad-token").error, AccountError::Unauthenticated);
    EXPECT_EQ(accounts->ResolveSession("pkt_" + std::string(64, '0')).error, AccountError::Unauthenticated);
    EXPECT_TRUE(accounts->Logout("pkt_" + std::string(64, '0')).Succeeded());
}

TEST_F(AccountBusiness, ExpiryBoundaryRejectsQueryAndRenewal)
{
    ASSERT_TRUE(accounts->CreateAccount({"alice", kPassword, ""}).Succeeded());
    const auto login = accounts->Login({"alice", kPassword});
    ASSERT_TRUE(login.Succeeded());
    now = kNow + 59;
    EXPECT_TRUE(accounts->ResolveSession(login.value->session_token).Succeeded());
    now = kNow + 60;
    EXPECT_EQ(accounts->ResolveSession(login.value->session_token).error, AccountError::SessionExpired);
    EXPECT_EQ(accounts->RenewSession(login.value->session_token).error, AccountError::SessionExpired);
    EXPECT_TRUE(accounts->Logout(login.value->session_token).Succeeded());
}

TEST(AccountPersistence, RegistrationPolicyIsIndependentOfServiceAvailability)
{
    auto options = Options(); options.registration_enabled = false;
    AccountService accounts(options, std::make_shared<SqliteAccountStore>(), std::make_shared<ScryptPasswordHasher>());
    ASSERT_TRUE(accounts.Start().Succeeded());
    EXPECT_EQ(accounts.CreateAccount({"alice", kPassword, ""}).error, AccountError::RegistrationDisabled);
}

TEST(AccountPersistence, RestartRetainsSessionsAndNeverStoresRawPasswordOrToken)
{
    Database db;
    AccountService accounts(Options(db.path), std::make_shared<SqliteAccountStore>(),
                            std::make_shared<ScryptPasswordHasher>(), [] { return kNow; });
    ASSERT_TRUE(accounts.Start().Succeeded());
    ASSERT_TRUE(accounts.CreateAccount({"alice", kPassword, ""}).Succeeded());
    const auto login = accounts.Login({"alice", kPassword});
    ASSERT_TRUE(login.Succeeded());
    const auto digest = db.Scalar("SELECT token_digest FROM account_sessions");
    EXPECT_EQ(digest.size(), 64u);
    EXPECT_NE(digest, login.value->session_token);
    EXPECT_NE(db.Scalar("SELECT password_hash FROM account_password_credentials"), kPassword);
    accounts.Stop();
    ASSERT_TRUE(accounts.Start().Succeeded());
    EXPECT_TRUE(accounts.ResolveSession(login.value->session_token).Succeeded());
    EXPECT_TRUE(accounts.Logout(login.value->session_token).Succeeded());
    accounts.Stop();
    ASSERT_TRUE(accounts.Start().Succeeded());
    EXPECT_EQ(accounts.ResolveSession(login.value->session_token).error, AccountError::SessionRevoked);
}

TEST(AccountPersistence, ConcurrentRenewalAcrossConnectionsHasExactlyOneWinner)
{
    Database db;
    AccountService first(Options(db.path), std::make_shared<SqliteAccountStore>(),
                         std::make_shared<ScryptPasswordHasher>(), [] { return kNow; });
    AccountService second(Options(db.path), std::make_shared<SqliteAccountStore>(),
                          std::make_shared<ScryptPasswordHasher>(), [] { return kNow; });
    ASSERT_TRUE(first.Start().Succeeded()); ASSERT_TRUE(second.Start().Succeeded());
    ASSERT_TRUE(first.CreateAccount({"alice", kPassword, ""}).Succeeded());
    const auto login = first.Login({"alice", kPassword}); ASSERT_TRUE(login.Succeeded());
    std::promise<void> release;
    const auto ready = release.get_future().share();
    std::vector<std::future<AccountResult<LoginResult>>> futures;
    for (int i = 0; i < 8; ++i)
        futures.push_back(std::async(std::launch::async, [&, i] {
            ready.wait(); return (i % 2 ? first : second).RenewSession(login.value->session_token);
        }));
    release.set_value();
    unsigned successes = 0;
    for (auto& future : futures)
    {
        const auto result = future.get();
        if (result.Succeeded()) { ++successes; EXPECT_TRUE(first.ResolveSession(result.value->session_token).Succeeded()); }
        else EXPECT_EQ(result.error, AccountError::SessionRevoked);
    }
    EXPECT_EQ(successes, 1u);
    EXPECT_EQ(db.Scalar("SELECT count(*) FROM account_sessions WHERE revoked_at=0"), "1");
}

TEST(AccountPersistence, ConcurrentRegistrationAcrossConnectionsHasOneUniqueLogin)
{
    Database db;
    SqliteAccountStore first, second;
    ASSERT_TRUE(first.Open(db.path).Succeeded()); ASSERT_TRUE(second.Open(db.path).Succeeded());
    StoredUser one{{"user1", "alice", "Alice", UserStatus::Active, 1, kNow}, "test-hash"};
    auto two = one; two.user.id = "user2";
    std::promise<void> release;
    const auto ready = release.get_future().share();
    auto a = std::async(std::launch::async, [&] { ready.wait(); return first.InsertUser(one); });
    auto b = std::async(std::launch::async, [&] { ready.wait(); return second.InsertUser(two); });
    release.set_value();
    const auto left = a.get(), right = b.get();
    EXPECT_NE(left.Succeeded(), right.Succeeded());
    EXPECT_EQ(left.Succeeded() ? right.error : left.error, AccountError::AlreadyExists);
    EXPECT_EQ(db.Scalar("SELECT count(*) FROM account_users"), "1");
    EXPECT_EQ(db.Scalar("SELECT count(*) FROM account_password_credentials"), "1");
}

class CallbackHasher final : public PasswordHasher
{
public:
    bool IsReady() const noexcept override { return real.IsReady(); }
    AccountResult<std::string> Hash(std::string_view value) const override { return real.Hash(value); }
    AccountStatus Verify(std::string_view value, std::string_view hash) const override
    {
        auto result = real.Verify(value, hash);
        if (result.Succeeded() && after_verify) after_verify();
        return result;
    }
    ScryptPasswordHasher real;
    std::function<void()> after_verify;
};

TEST(AccountPersistence, RechecksDisabledUserAndCredentialVersionAfterPasswordVerification)
{
    Database db;
    const auto hasher = std::make_shared<CallbackHasher>();
    AccountService accounts(Options(db.path), std::make_shared<SqliteAccountStore>(), hasher, [] { return kNow; });
    ASSERT_TRUE(accounts.Start().Succeeded());
    ASSERT_TRUE(accounts.CreateAccount({"alice", kPassword, ""}).Succeeded());
    const auto prior = accounts.Login({"alice", kPassword}); ASSERT_TRUE(prior.Succeeded());
    hasher->after_verify = [&] { db.Exec("UPDATE account_users SET status=1"); };
    EXPECT_EQ(accounts.Login({"alice", kPassword}).error, AccountError::UserDisabled);
    EXPECT_EQ(accounts.ResolveSession(prior.value->session_token).error, AccountError::UserDisabled);
    EXPECT_EQ(accounts.RenewSession(prior.value->session_token).error, AccountError::UserDisabled);
    db.Exec("UPDATE account_users SET status=0");
    hasher->after_verify = [&] { db.Exec("UPDATE account_users SET credential_version=credential_version+1"); };
    EXPECT_EQ(accounts.Login({"alice", kPassword}).error, AccountError::InvalidCredentials);
    EXPECT_EQ(accounts.ResolveSession(prior.value->session_token).error, AccountError::SessionRevoked);
    EXPECT_EQ(accounts.RenewSession(prior.value->session_token).error, AccountError::SessionRevoked);
    EXPECT_EQ(db.Scalar("SELECT count(*) FROM account_sessions"), "1");
}

TEST(AccountPersistence, FailedCredentialInsertAndFailedRotationRollBackAllChanges)
{
    Database db;
    SqliteAccountStore store;
    ASSERT_TRUE(store.Open(db.path).Succeeded());
    db.Exec("CREATE TRIGGER reject_credential BEFORE INSERT ON account_password_credentials "
            "BEGIN SELECT RAISE(ABORT,'injected failure'); END");
    const StoredUser user{{"user1", "alice", "Alice", UserStatus::Active, 1, kNow}, "test-hash"};
    EXPECT_EQ(store.InsertUser(user).error, AccountError::StorageError);
    EXPECT_EQ(store.FindUserByLogin("alice").error, AccountError::NotFound);
    EXPECT_EQ(db.Scalar("SELECT count(*) FROM account_users"), "0");
    db.Exec("DROP TRIGGER reject_credential");
    ASSERT_TRUE(store.InsertUser(user).Succeeded());
    const LoginSession previous{"session1", "user1", std::string(64, 'a'), 1, kNow, kNow + 60, 0};
    const LoginSession existing{"session2", "user1", std::string(64, 'b'), 1, kNow, kNow + 60, 0};
    ASSERT_TRUE(store.InsertSessionIfCurrent(previous, kNow).Succeeded());
    ASSERT_TRUE(store.InsertSessionIfCurrent(existing, kNow).Succeeded());
    auto collision = existing; collision.token_digest = std::string(64, 'c');
    EXPECT_EQ(store.RotateSessionIfCurrent(previous.token_digest, collision, kNow).error, AccountError::StorageError);
    auto retained = store.FindSessionByDigest(previous.token_digest);
    ASSERT_TRUE(retained.Succeeded()); EXPECT_EQ(retained.value->session.revoked_at_unix_seconds, 0);
    EXPECT_EQ(store.FindSessionByDigest(collision.token_digest).error, AccountError::NotFound);
    auto other_user = collision; other_user.user_id = "other";
    EXPECT_EQ(store.RotateSessionIfCurrent(previous.token_digest, other_user, kNow).error, AccountError::InvalidArgument);
    EXPECT_EQ(db.Scalar("SELECT count(*) FROM account_sessions"), "2");
}

TEST(AccountPersistence, SchemaVersionIsNamespacedAndFutureVersionsFailClosed)
{
    Database db;
    db.Exec("PRAGMA user_version=7");
    SqliteAccountStore store;
    ASSERT_TRUE(store.Open(db.path).Succeeded());
    EXPECT_EQ(db.Scalar("PRAGMA user_version"), "7");
    store.Close();
    db.Exec("UPDATE account_schema_version SET version=99");
    EXPECT_EQ(store.Open(db.path).error, AccountError::StorageError);
    EXPECT_EQ(store.FindUserByLogin("alice").error, AccountError::Unavailable);
    db.Exec("UPDATE account_schema_version SET version=1");
    EXPECT_TRUE(store.Open(db.path).Succeeded());
}

}
}
