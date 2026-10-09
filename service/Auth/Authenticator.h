#ifndef PACKETIA_SERVICE_AUTH_AUTHENTICATOR_H_
#define PACKETIA_SERVICE_AUTH_AUTHENTICATOR_H_

#include "AuthTypes.h"
#include <string_view>

namespace service::auth
{

class IAuthenticator
{
public:
    virtual ~IAuthenticator() = default;

    virtual AuthResult Authenticate(std::string_view token, Audience expected_audience) const = 0;
};

}

#endif // PACKETIA_SERVICE_AUTH_AUTHENTICATOR_H_
