#ifndef PACKETIA_SERVICE_ACCOUNT_SCRYPT_PASSWORD_HASHER_H_
#define PACKETIA_SERVICE_ACCOUNT_SCRYPT_PASSWORD_HASHER_H_

#include "PasswordHasher.h"

namespace service::account
{

// OpenSSL scrypt, random 128-bit salt and bounded, self-describing parameters.
class ScryptPasswordHasher final : public PasswordHasher
{
public:
    bool IsReady() const noexcept override;
    AccountResult<std::string> Hash(std::string_view password) const override;
    AccountStatus Verify(std::string_view password,
                          std::string_view encoded_hash) const override;
};

}

#endif // PACKETIA_SERVICE_ACCOUNT_SCRYPT_PASSWORD_HASHER_H_
