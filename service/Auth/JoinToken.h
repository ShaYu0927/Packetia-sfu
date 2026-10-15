#pragma once
#include "AuthTypes.h"
#include <string>

namespace service::auth
{
// Call only after business authorization. These values are never client grants.
std::string SignJoinToken(const AuthContext& context, const std::string& secret, int64_t issued_at);
}
