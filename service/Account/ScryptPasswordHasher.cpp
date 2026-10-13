#include "ScryptPasswordHasher.h"

namespace service::account
{

bool ScryptPasswordHasher::IsReady() const noexcept
{
    return false;
}

AccountResult<std::string> ScryptPasswordHasher::Hash(std::string_view) const
{
    // TODO: Use a vetted scrypt implementation with bounded work parameters.
    return AccountResult<std::string>::Failure(AccountError::NotImplemented);
}

AccountStatus ScryptPasswordHasher::Verify(std::string_view, std::string_view) const
{
    // TODO: Parse and bound stored parameters, derive and compare in constant time.
    return AccountStatus::Failure(AccountError::NotImplemented);
}

}
