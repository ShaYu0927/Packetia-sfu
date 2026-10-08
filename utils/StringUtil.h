#ifndef PACKETIA_UTILS_STRING_UTIL_H_
#define PACKETIA_UTILS_STRING_UTIL_H_

#include <string>
#include <string_view>

namespace utils
{

inline bool IsPrintableAscii(std::string_view value)
{
    for (unsigned char c : value)
        if (c < 0x20 || c > 0x7E) return false;
    return true;
}

inline std::string ToLowerAscii(std::string value)
{
    for (char& c : value)
        if (c >= 'A' && c <= 'Z') c = static_cast<char>(c + ('a' - 'A'));
    return value;
}

inline std::string_view TrimSpaceAndTab(std::string_view value)
{
    const auto begin = value.find_first_not_of(" \t");
    if (begin == std::string_view::npos) return {};
    const auto end = value.find_last_not_of(" \t");
    return value.substr(begin, end - begin + 1);
}

inline bool IsTokenCharacter(char c)
{
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
        (c >= '0' && c <= '9') ||
        std::string_view("!#$%&'*+-.^_`|~").find(c) != std::string_view::npos;
}

} // namespace utils

#endif // PACKETIA_UTILS_STRING_UTIL_H_
