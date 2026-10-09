#include "service/Auth/AuthService.h"
#include <gtest/gtest.h>

namespace service::auth
{
namespace
{
class StubAuthenticator final : public IAuthenticator
{
public:
    AuthResult result;
    AuthResult Authenticate(std::string_view, Audience) const override { return result; }
};

class StubAccessPolicy final : public IAccessPolicy
{
public:
    AuthDecision decision;
    mutable unsigned calls = 0;

    AuthDecision Check(const AuthContext&, Action, const ResourceContext&) const override
    {
        ++calls;
        return decision;
    }
};

AuthContext Identity()
{
    AuthContext context;
    context.subject_id = "user-a";
    context.expires_at_unix_seconds = 200;
    return context;
}

TEST(AuthFramework, MissingProvidersAndDefaultResultsDenyAccess)
{
    AuthService auth(nullptr, nullptr, [] { return 100; });
    EXPECT_EQ(auth.Authenticate("token", Audience::WebRtc).error, AuthError::NotConfigured);
    EXPECT_EQ(auth.Authorize(Identity(), Action::JoinRoom, {}).error, AuthError::NotConfigured);
    EXPECT_FALSE(AuthResult{}.Succeeded());
    EXPECT_FALSE(AuthDecision{}.Allowed());
}

TEST(AuthFramework, RejectedVerificationCannotExposeAnIdentity)
{
    auto authenticator = std::make_shared<StubAuthenticator>();
    authenticator->result = {AuthError::InvalidToken, Identity()};
    AuthService auth(authenticator, nullptr, [] { return 100; });
    const auto result = auth.Authenticate("invalid", Audience::WebRtc);
    EXPECT_EQ(result.error, AuthError::InvalidToken);
    EXPECT_FALSE(result.context.has_value());
    authenticator->result = {AuthError::None, std::nullopt};
    EXPECT_EQ(auth.Authenticate("invalid", Audience::WebRtc).error, AuthError::InternalError);
}

TEST(AuthFramework, EmptyCredentialInvalidIdentityAndWrongAudienceAreRejected)
{
    auto authenticator = std::make_shared<StubAuthenticator>();
    authenticator->result = AuthResult::Success(Identity());
    AuthService auth(authenticator, nullptr, [] { return 100; });
    EXPECT_EQ(auth.Authenticate("", Audience::WebRtc).error, AuthError::Unauthenticated);
    EXPECT_EQ(auth.Authenticate("token", Audience::Control).error, AuthError::WrongAudience);
    authenticator->result.context->subject_id.clear();
    EXPECT_EQ(auth.Authenticate("token", Audience::WebRtc).error, AuthError::InvalidToken);
    authenticator->result = AuthResult::Success(Identity());
    authenticator->result.context->expires_at_unix_seconds = 0;
    EXPECT_EQ(auth.Authenticate("token", Audience::WebRtc).error, AuthError::InvalidToken);
}

TEST(AuthFramework, StoredIdentityExpiresBeforeThePolicyCanAllowAnOperation)
{
    int64_t now = 100;
    auto authenticator = std::make_shared<StubAuthenticator>();
    authenticator->result = AuthResult::Success(Identity());
    auto policy = std::make_shared<StubAccessPolicy>();
    policy->decision = AuthDecision::Allow();
    AuthService auth(authenticator, policy, [&] { return now; });
    const auto result = auth.Authenticate("token", Audience::WebRtc);
    ASSERT_TRUE(result.Succeeded());
    now = 200;
    EXPECT_EQ(auth.Authorize(*result.context, Action::Publish, {}).error, AuthError::ExpiredToken);
    EXPECT_EQ(policy->calls, 0u);
    EXPECT_EQ(auth.Authenticate("token", Audience::WebRtc).error, AuthError::ExpiredToken);
}

TEST(AuthFramework, ValidIdentityStillRequiresAnExplicitPolicyDecision)
{
    auto authenticator = std::make_shared<StubAuthenticator>();
    authenticator->result = AuthResult::Success(Identity());
    auto policy = std::make_shared<StubAccessPolicy>();
    AuthService auth(authenticator, policy, [] { return 100; });
    const auto result = auth.Authenticate("token", Audience::WebRtc);
    ASSERT_TRUE(result.Succeeded());
    EXPECT_FALSE(auth.Authorize(*result.context, Action::Publish, {}).Allowed());
    policy->decision = AuthDecision::Allow();
    EXPECT_TRUE(auth.Authorize(*result.context, Action::Publish, {}).Allowed());
}
}
}
