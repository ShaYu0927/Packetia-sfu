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
    AuthResult Authenticate(const AuthRequest& request, Audience expected_audience) const;
    // caller 必须来自已验证的身份快照；本方法不重新验证凭据或检查到期时间。
    // WebRTC 在 join/resume 时认证；RTSP 在每条受保护命令进入时重新认证。
    AuthDecision Authorize(const AuthContext& caller, Action action, const ResourceContext& resource) const;

private:
    AuthError ValidateContext(const AuthContext& caller, bool check_expiry) const;

    const std::shared_ptr<const IAuthenticator> authenticator_;
    const std::shared_ptr<const IAccessPolicy> policy_;
    const Clock clock_;
};

}

#endif // PACKETIA_SERVICE_AUTH_SERVICE_H_
