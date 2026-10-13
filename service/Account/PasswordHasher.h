#ifndef PACKETIA_SERVICE_ACCOUNT_PASSWORD_HASHER_H_
#define PACKETIA_SERVICE_ACCOUNT_PASSWORD_HASHER_H_

#include "AccountTypes.h"

#include <string_view>

namespace service::account
{

// Implementations are safe for concurrent callers and perform no network I/O.
// Encoded hashes include the algorithm, work parameters, random salt and hash.
class PasswordHasher
{
public:
    virtual ~PasswordHasher() = default;
    virtual bool IsReady() const noexcept = 0;
    virtual AccountResult<std::string> Hash(std::string_view password) const = 0;
    // InvalidCredentials means mismatch; unavailable implementation must fail.
    virtual AccountStatus Verify(std::string_view password,
                                  std::string_view encoded_hash) const = 0;
};

}

#endif // PACKETIA_SERVICE_ACCOUNT_PASSWORD_HASHER_H_
