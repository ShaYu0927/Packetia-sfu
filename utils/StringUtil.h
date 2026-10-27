#ifndef PACKETIA_UTILS_STRING_UTIL_H_
#define PACKETIA_UTILS_STRING_UTIL_H_

#include <cctype>
#include <string>
#include <string_view>

namespace utils
{

// 去除首尾空白字符，沿用标准字符分类规则。
inline void Trim(std::string& value)
{
    while (!value.empty() && std::isspace(static_cast<unsigned char>(value.front())))
        value.erase(value.begin());
    while (!value.empty() && std::isspace(static_cast<unsigned char>(value.back())))
        value.pop_back();
}

// 按首次出现的分隔符拆分；未找到时不修改输出。
inline bool SplitOnce(const std::string& value, char delimiter,
                      std::string& left, std::string& right)
{
    const auto pos = value.find(delimiter);
    if (pos == std::string::npos) return false;
    left = value.substr(0, pos);
    right = value.substr(pos + 1);
    return true;
}

// 使用当前 C locale 的转换规则；仅处理 ASCII 时可使用 ToLowerAscii。
inline std::string ToLower(std::string value)
{
    for (char& c : value)
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return value;
}

// 按 ASCII 规则进行大小写不敏感的前缀比较。
inline bool IStartsWith(std::string_view value, std::string_view prefix)
{
    if (value.size() < prefix.size()) return false;
    for (std::size_t i = 0; i < prefix.size(); ++i)
    {
        unsigned char a = static_cast<unsigned char>(value[i]);
        unsigned char b = static_cast<unsigned char>(prefix[i]);
        if (a >= 'A' && a <= 'Z') a = a - 'A' + 'a';
        if (b >= 'A' && b <= 'Z') b = b - 'A' + 'a';
        if (a != b) return false;
    }
    return true;
}

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
