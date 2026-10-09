#ifndef PACKETIA_SERVICE_AUTH_SERVICE_H_
#define PACKETIA_SERVICE_AUTH_SERVICE_H_

#include "AuthAccessPolicy.h"
#include "Authenticator.h"
#include <functional>
#include <memory>

namespace service::auth
{

class AuthService final
{
public:
    using Clock = std::function<int64_t()>; // Unix seconds, not monotonic time.

    AuthService(std::shared_ptr<const IAuthenticator> authenticator, std::shared_ptr<const IAccessPolicy> policy, Clock clock = {});

    AuthResult Authenticate(std::string_view token, Audience expected_audience) const;
    AuthDecision Authorize(const AuthContext& caller, Action action, const ResourceContext& resource) const;

private:
    AuthError ValidateContext(const AuthContext& caller) const;

    const std::shared_ptr<const IAuthenticator> authenticator_;
    const std::shared_ptr<const IAccessPolicy> policy_;
    const Clock clock_;
};

}

#endif // PACKETIA_SERVICE_AUTH_SERVICE_H_
