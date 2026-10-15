#include "AccountService.h"
#include "AccountPolicy.h"

#include <openssl/evp.h>
#include <openssl/rand.h>
#include <array>
#include <chrono>
#include <limits>
#include <mutex>
#include <stdexcept>
#include <utility>

namespace service::account
{
namespace
{
int64_t UnixSeconds()
{
    return std::chrono::duration_cast<std::chrono::seconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();
}

std::string Hex(const unsigned char* bytes, size_t size)
{
    constexpr char alphabet[] = "0123456789abcdef";
    std::string result(size * 2, '0');
    for (size_t i = 0; i < size; ++i)
    {
        result[i * 2] = alphabet[bytes[i] >> 4];
        result[i * 2 + 1] = alphabet[bytes[i] & 15];
    }
    return result;
}

std::string RandomHex(int bytes)
{
    std::array<unsigned char, 32> random{};
    if (bytes > static_cast<int>(random.size()) || RAND_bytes(random.data(), bytes) != 1)
        throw std::runtime_error("account random generation failed");
    return Hex(random.data(), static_cast<size_t>(bytes));
}

AccountResult<std::string> TokenDigest(std::string_view token)
{
    if (token.size() != 68 || token.substr(0, 4) != "pkt_")
        return AccountResult<std::string>::Failure(AccountError::Unauthenticated);
    for (char ch : token.substr(4))
        if (!((ch >= '0' && ch <= '9') || (ch >= 'a' && ch <= 'f')))
            return AccountResult<std::string>::Failure(AccountError::Unauthenticated);
    std::array<unsigned char, 32> digest{};
    unsigned size = 0;
    if (EVP_Digest(token.data(), token.size(), digest.data(), &size, EVP_sha256(), nullptr) != 1 ||
        size != digest.size())
        return AccountResult<std::string>::Failure(AccountError::InternalError);
    return AccountResult<std::string>::Success(Hex(digest.data(), digest.size()));
}

AccountIdentity Identity(const StoredSession& stored)
{
    return {stored.user.id, stored.session.session_id, stored.user.display_name};
}

AccountStatus NewSession(const User& user, int64_t now, uint32_t ttl, LoginSession& session, LoginResult& result)
{
    if (now <= 0 || now > std::numeric_limits<int64_t>::max() - ttl)
        return AccountStatus::Failure(AccountError::InternalError);
    result.session_token = "pkt_" + RandomHex(32);
    const auto digest = TokenDigest(result.session_token);
    if (!digest.Succeeded()) return AccountStatus::Failure(digest.error);
    session = {RandomHex(16), user.id, *digest.value, user.credential_version, now, now + ttl, 0};
    result.identity = {user.id, session.session_id, user.display_name};
    result.expires_at_unix_seconds = session.expires_at_unix_seconds;
    return AccountStatus::Success();
}
}

AccountService::AccountService(AccountOptions options, std::shared_ptr<AccountStore> store,
                               std::shared_ptr<PasswordHasher> password_hasher, Clock clock)
    : options_(std::move(options)), store_(std::move(store)),
      password_hasher_(std::move(password_hasher)), clock_(clock ? std::move(clock) : Clock{UnixSeconds})
{
}

AccountService::~AccountService() { Stop(); }

AccountStatus AccountService::Start()
{
    std::unique_lock<std::shared_mutex> lock(mutex_);
    if (!options_.enabled || !store_ || !password_hasher_)
        return AccountStatus::Failure(AccountError::NotConfigured);
    if (!options_.IsValid()) return AccountStatus::Failure(AccountError::InvalidArgument);
    if (started_) return AccountStatus::Success();
    if (!password_hasher_->IsReady()) return AccountStatus::Failure(AccountError::Unavailable);
    try
    {
        // Missing accounts still execute password verification with the same KDF.
        const auto dummy = password_hasher_->Hash(RandomHex(32));
        if (!dummy.Succeeded()) return AccountStatus::Failure(dummy.error);
        const auto result = store_->Open(options_.database_path);
        if (!result.Succeeded())
        {
            store_->Close();
            return result;
        }
        dummy_password_hash_ = *dummy.value;
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
    if (!started_) return;
    started_ = false;
    dummy_password_hash_.clear();
    store_->Close();
}

AccountError AccountService::CheckReadyLocked() const
{
    if (!options_.enabled || !store_ || !password_hasher_) return AccountError::NotConfigured;
    return started_ ? AccountError::None : AccountError::Unavailable;
}

AccountResult<User> AccountService::CreateAccount(const CreateAccountRequest& request)
{
    std::shared_lock<std::shared_mutex> lock(mutex_);
    const auto error = CheckReadyLocked();
    if (error != AccountError::None) return AccountResult<User>::Failure(error);
    if (!options_.registration_enabled)
        return AccountResult<User>::Failure(AccountError::RegistrationDisabled);
    try
    {
        std::string login;
        if (!NormalizeLogin(request.login, login) || !ValidPasswordInput(request.password) ||
            request.password.size() < 12 || !ValidDisplayName(request.display_name))
            return AccountResult<User>::Failure(AccountError::InvalidArgument);
        const auto hash = password_hasher_->Hash(request.password);
        if (!hash.Succeeded()) return AccountResult<User>::Failure(hash.error);
        const auto now = clock_();
        if (now <= 0) return AccountResult<User>::Failure(AccountError::InternalError);
        User user{RandomHex(16), login, request.display_name.empty() ? login : request.display_name,
                  UserStatus::Active, 1, now};
        const auto inserted = store_->InsertUser({user, *hash.value});
        if (!inserted.Succeeded()) return AccountResult<User>::Failure(inserted.error);
        return AccountResult<User>::Success(std::move(user));
    }
    catch (...) { return AccountResult<User>::Failure(AccountError::InternalError); }
}

AccountResult<LoginResult> AccountService::Login(const LoginRequest& request)
{
    std::shared_lock<std::shared_mutex> lock(mutex_);
    const auto error = CheckReadyLocked();
    if (error != AccountError::None) return AccountResult<LoginResult>::Failure(error);
    try
    {
        std::string login;
        if (!NormalizeLogin(request.login, login) || !ValidPasswordInput(request.password))
            return AccountResult<LoginResult>::Failure(AccountError::InvalidArgument);
        const auto found = store_->FindUserByLogin(login);
        if (!found.Succeeded() && found.error != AccountError::NotFound)
            return AccountResult<LoginResult>::Failure(found.error);
        const auto verified = password_hasher_->Verify(request.password,
            found.Succeeded() ? found.value->password_hash : dummy_password_hash_);
        if (!verified.Succeeded() && verified.error != AccountError::InvalidCredentials)
            return AccountResult<LoginResult>::Failure(verified.error);
        if (!found.Succeeded() || !verified.Succeeded())
            return AccountResult<LoginResult>::Failure(AccountError::InvalidCredentials);
        const auto& user = found.value->user;
        if (user.status != UserStatus::Active)
            return AccountResult<LoginResult>::Failure(AccountError::UserDisabled);
        LoginSession session;
        LoginResult result;
        const auto now = clock_();
        const auto generated = NewSession(user, now, options_.session_ttl_seconds, session, result);
        if (!generated.Succeeded()) return AccountResult<LoginResult>::Failure(generated.error);
        // Recheck status/version after expensive password verification in a write transaction.
        const auto inserted = store_->InsertSessionIfCurrent(session, now);
        if (!inserted.Succeeded()) return AccountResult<LoginResult>::Failure(inserted.error);
        return AccountResult<LoginResult>::Success(std::move(result));
    }
    catch (...) { return AccountResult<LoginResult>::Failure(AccountError::InternalError); }
}

AccountResult<AccountIdentity> AccountService::ResolveSession(std::string_view token)
{
    std::shared_lock<std::shared_mutex> lock(mutex_);
    const auto error = CheckReadyLocked();
    if (error != AccountError::None) return AccountResult<AccountIdentity>::Failure(error);
    try
    {
        const auto digest = TokenDigest(token);
        if (!digest.Succeeded()) return AccountResult<AccountIdentity>::Failure(digest.error);
        const auto stored = store_->FindSessionByDigest(*digest.value);
        if (!stored.Succeeded()) return AccountResult<AccountIdentity>::Failure(
            stored.error == AccountError::NotFound ? AccountError::Unauthenticated : stored.error);
        const auto valid = SessionValidity(*stored.value, clock_());
        if (valid != AccountError::None) return AccountResult<AccountIdentity>::Failure(valid);
        return AccountResult<AccountIdentity>::Success(Identity(*stored.value));
    }
    catch (...) { return AccountResult<AccountIdentity>::Failure(AccountError::InternalError); }
}

AccountResult<LoginResult> AccountService::RenewSession(std::string_view token)
{
    std::shared_lock<std::shared_mutex> lock(mutex_);
    const auto error = CheckReadyLocked();
    if (error != AccountError::None) return AccountResult<LoginResult>::Failure(error);
    try
    {
        const auto digest = TokenDigest(token);
        if (!digest.Succeeded()) return AccountResult<LoginResult>::Failure(digest.error);
        const auto stored = store_->FindSessionByDigest(*digest.value);
        if (!stored.Succeeded()) return AccountResult<LoginResult>::Failure(
            stored.error == AccountError::NotFound ? AccountError::Unauthenticated : stored.error);
        const auto now = clock_();
        const auto valid = SessionValidity(*stored.value, now);
        if (valid != AccountError::None) return AccountResult<LoginResult>::Failure(valid);
        LoginSession replacement;
        LoginResult result;
        const auto generated = NewSession(stored.value->user, now, options_.session_ttl_seconds,
                                           replacement, result);
        if (!generated.Succeeded()) return AccountResult<LoginResult>::Failure(generated.error);
        const auto rotated = store_->RotateSessionIfCurrent(*digest.value, replacement, now);
        if (!rotated.Succeeded()) return AccountResult<LoginResult>::Failure(rotated.error);
        return AccountResult<LoginResult>::Success(std::move(result));
    }
    catch (...) { return AccountResult<LoginResult>::Failure(AccountError::InternalError); }
}

AccountStatus AccountService::Logout(std::string_view token)
{
    std::shared_lock<std::shared_mutex> lock(mutex_);
    const auto error = CheckReadyLocked();
    if (error != AccountError::None) return AccountStatus::Failure(error);
    try
    {
        const auto digest = TokenDigest(token);
        if (!digest.Succeeded()) return AccountStatus::Failure(digest.error);
        const auto now = clock_();
        if (now <= 0) return AccountStatus::Failure(AccountError::InternalError);
        return store_->RevokeSession(*digest.value, now);
    }
    catch (...) { return AccountStatus::Failure(AccountError::InternalError); }
}

}
