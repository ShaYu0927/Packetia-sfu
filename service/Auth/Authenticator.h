#ifndef PACKETIA_SERVICE_AUTH_AUTHENTICATOR_H_
#define PACKETIA_SERVICE_AUTH_AUTHENTICATOR_H_

#include "AuthTypes.h"
#include <string_view>

namespace service::auth
{

// 请求字段仅在同步认证调用期间借用；Digest 必须绑定原始方法和 URI。
struct AuthRequest
{
    std::string_view credential;
    std::string_view method;
    std::string_view uri;
};

class IAuthenticator
{
public:
    virtual ~IAuthenticator() = default;

    virtual AuthResult Authenticate(std::string_view token, Audience expected_audience) const = 0;
    virtual AuthResult Authenticate(const AuthRequest& request, Audience expected_audience) const
    { return Authenticate(request.credential, expected_audience); }
};

}

#endif // PACKETIA_SERVICE_AUTH_AUTHENTICATOR_H_
