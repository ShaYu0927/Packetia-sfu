#include "JwtAuthenticator.h"

namespace service::auth
{
    
AuthResult JwtAuthenticator::Authenticate(std::string_view token, Audience expected_audience) const
{
    if (public_key_.empty() || issuer_.empty())
        return AuthResult::Failure(AuthError::NotConfigured);

    if (token.empty())
        return AuthResult::Failure(AuthError::Unauthenticated);

    const char* audience = nullptr;
    switch (expected_audience)
    {
    case Audience::WebRtc:
        audience = "webrtc";
        break;
    case Audience::Control:
        audience = "control";
        break;
    default:
        return AuthResult::Failure(AuthError::WrongAudience);
    }

    try
    {
        const auto decoded = jwt::decode(std::string(token));

        if (!decoded.has_expires_at() || !decoded.has_subject())
            return AuthResult::Failure(AuthError::InvalidToken);

        // 限定 RS256，并校验签名、签发者、受众及时间声明。
        auto verifier = jwt::verify()
            .allow_algorithm(jwt::algorithm::rs256(public_key_, "", "", ""))
            .with_issuer(issuer_)
            .with_audience(audience);

        std::error_code ec;
        verifier.verify(decoded, ec);

        using Error = jwt::error::token_verification_error;

        if (ec == Error::audience_missmatch)
            return AuthResult::Failure(AuthError::WrongAudience);

        if (ec)
        {
            if (ec == Error::token_expired &&
                std::chrono::system_clock::now() >= decoded.get_expires_at())
            {
                return AuthResult::Failure(AuthError::ExpiredToken);
            }

            return AuthResult::Failure(AuthError::InvalidToken);
        }

        // 验证成功后，提取可信的身份信息。
        AuthContext context;
        context.subject_id = decoded.get_subject();

        if (context.subject_id.empty())
            return AuthResult::Failure(AuthError::InvalidToken);

        context.audience = expected_audience;
        context.token_id = decoded.has_id() ? decoded.get_id() : "";
        context.expires_at_unix_seconds =
            std::chrono::duration_cast<std::chrono::seconds>(
                decoded.get_expires_at().time_since_epoch()).count();

        return AuthResult::Success(std::move(context));
    }
    catch (const std::exception&)
    {
        return AuthResult::Failure(AuthError::InvalidToken);
    }
}

}
