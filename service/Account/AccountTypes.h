#ifndef PACKETIA_SERVICE_ACCOUNT_TYPES_H_
#define PACKETIA_SERVICE_ACCOUNT_TYPES_H_

#include <cstdint>
#include <optional>
#include <string>
#include <utility>

namespace service::account
{

enum class AccountError
{
    None,
    InvalidArgument,
    Unauthenticated,
    InvalidCredentials,
    AlreadyExists,
    NotFound,
    UserDisabled,
    SessionExpired,
    SessionRevoked,
    NotConfigured,
    NotImplemented,
    Unavailable,
    StorageError,
    InternalError
};

struct AccountStatus
{
    AccountError error = AccountError::InternalError;

    bool Succeeded() const { return error == AccountError::None; }

    static AccountStatus Success() { return {AccountError::None}; }
    static AccountStatus Failure(AccountError error)
    {
        return {error == AccountError::None ? AccountError::InternalError : error};
    }
};

template <typename T>
struct AccountResult
{
    AccountError error = AccountError::InternalError;
    std::optional<T> value;

    bool Succeeded() const { return error == AccountError::None && value.has_value(); }

    static AccountResult Success(T value)
    {
        return {AccountError::None, std::move(value)};
    }

    static AccountResult Failure(AccountError error)
    {
        return {error == AccountError::None ? AccountError::InternalError : error, std::nullopt};
    }
};

enum class UserStatus { Active, Disabled };

// Safe to return to the API layer: contains no password hash or session secret.
struct User
{
    std::string id;
    std::string login;
    std::string display_name;
    UserStatus status = UserStatus::Active;
    uint64_t credential_version = 1;
    int64_t created_at_unix_seconds = 0;
};

struct StoredUser
{
    User user;
    std::string password_hash;
};

struct LoginSession
{
    std::string session_id;
    std::string user_id;
    std::string token_digest; // The raw bearer credential must never be persisted.
    uint64_t credential_version = 0;
    int64_t created_at_unix_seconds = 0;
    int64_t expires_at_unix_seconds = 0;
    int64_t revoked_at_unix_seconds = 0; // Zero means not revoked.
};

// Read the user and session in one database snapshot before checking validity.
struct StoredSession
{
    LoginSession session;
    User user;
};

struct AccountIdentity
{
    std::string user_id;
    std::string session_id;
    std::string display_name;
};

struct CreateAccountRequest
{
    std::string login;
    std::string password;
    std::string display_name;
};

struct LoginRequest
{
    std::string login;
    std::string password;
};

struct LoginResult
{
    AccountIdentity identity;
    std::string session_token; // Return to the client; do not log or persist.
    int64_t expires_at_unix_seconds = 0;
};

}

#endif // PACKETIA_SERVICE_ACCOUNT_TYPES_H_
