#include "AuthService.h"

#include <chrono>
#include <utility>

namespace service::auth
{
namespace
{
int64_t UnixSeconds()
{
    return std::chrono::duration_cast<std::chrono::seconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();
}
}

AuthService::AuthService(std::shared_ptr<const IAuthenticator> authenticator,
                         std::shared_ptr<const IAccessPolicy> policy, Clock clock)
    : authenticator_(std::move(authenticator)), policy_(std::move(policy)),
      clock_(clock ? std::move(clock) : Clock{UnixSeconds})
{
}

AuthError AuthService::ValidateContext(const AuthContext& caller) const
{
    if (caller.subject_id.empty() || caller.expires_at_unix_seconds <= 0)
        return AuthError::InvalidToken;
    if (clock_() >= caller.expires_at_unix_seconds) return AuthError::ExpiredToken;
    return AuthError::None;
}

AuthResult AuthService::Authenticate(std::string_view token, Audience expected_audience) const
{
    if (!authenticator_) return AuthResult::Failure(AuthError::NotConfigured);
    if (token.empty()) return AuthResult::Failure(AuthError::Unauthenticated);
    auto result = authenticator_->Authenticate(token, expected_audience);
    if (!result.Succeeded())
        return AuthResult::Failure(result.error);
    if (result.context->audience != expected_audience)
        return AuthResult::Failure(AuthError::WrongAudience);
    const auto error = ValidateContext(*result.context);
    if (error != AuthError::None) return AuthResult::Failure(error);
    return result;
}

AuthDecision AuthService::Authorize(const AuthContext& caller, Action action, const ResourceContext& resource) const
{
    if (!policy_) return AuthDecision::Deny(AuthError::NotConfigured);
    const auto error = ValidateContext(caller);
    if (error != AuthError::None) return AuthDecision::Deny(error);
    return policy_->Check(caller, action, resource);
}

}
