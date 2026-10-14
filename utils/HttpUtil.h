#ifndef PACKETIA_UTILS_HTTP_UTIL_H_
#define PACKETIA_UTILS_HTTP_UTIL_H_

#include "StringUtil.h"

#include <string_view>

namespace utils
{

// HTTP field names are nonempty RFC tokens, before or after normalization.
inline bool IsValidHttpHeaderName(std::string_view value) noexcept
{
    if (value.empty()) return false;
    for (char c : value)
        if (!IsTokenCharacter(c)) return false;
    return true;
}

// Allow HTAB and obs-text, but reject CR/LF, other controls and DEL.
inline bool IsValidHttpHeaderValue(std::string_view value) noexcept
{
    for (unsigned char c : value)
        if ((c < 32 && c != '\t') || c == 127) return false;
    return true;
}

} // namespace utils

#endif // PACKETIA_UTILS_HTTP_UTIL_H_
