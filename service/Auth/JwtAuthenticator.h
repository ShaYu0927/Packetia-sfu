#ifndef PACKETIA_SERVICE_AUTH_JWT_AUTHENTICATOR_H_
#define PACKETIA_SERVICE_AUTH_JWT_AUTHENTICATOR_H_

#include "Authenticator.h"
#include "KeyProvider.h"
#include <functional>
#include <memory>

namespace service::auth
{

class JwtAuthenticator final : public IAuthenticator
{
public:
    using Clock = std::function<int64_t()>; // Unix seconds.

    explicit JwtAuthenticator(std::shared_ptr<const IKeyProvider> keys, Clock clock = {});
    AuthResult Authenticate(std::string_view token, Audience expected_audience) const override;

private:
    const std::shared_ptr<const IKeyProvider> keys_;
    const Clock clock_;
};

}

#endif // PACKETIA_SERVICE_AUTH_JWT_AUTHENTICATOR_H_
