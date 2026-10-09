#ifndef PACKETIA_SERVICE_AUTH_JWT_AUTHENTICATOR_H_
#define PACKETIA_SERVICE_AUTH_JWT_AUTHENTICATOR_H_

#include "Authenticator.h"

namespace service::auth
{

class JwtAuthenticator final : public IAuthenticator
{
public:
    // Implement in JwtAuthenticator.cpp using a JWT verification library.
    AuthResult Authenticate(std::string_view token, Audience expected_audience) const override;
};

}

#endif // PACKETIA_SERVICE_AUTH_JWT_AUTHENTICATOR_H_
