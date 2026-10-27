#ifndef PACKETIA_UTILS_HTTP_UTIL_H_
#define PACKETIA_UTILS_HTTP_UTIL_H_

#include "StringUtil.h"

#include <cstddef>
#include <string_view>

namespace utils
{

// 查找 HTTP/RTSP 报文头结束标记；未找到时返回 -1。
inline int FindCrlfCrlf(const char* data, std::size_t size)
{
    for (std::size_t i = 0; i + 3 < size; ++i)
    {
        if (data[i] == '\r' && data[i + 1] == '\n' &&
            data[i + 2] == '\r' && data[i + 3] == '\n')
            return static_cast<int>(i);
    }
    return -1;
}

// 从 CRLF 分隔的报文头提取正文长度；缺少该字段时长度为零。
inline bool ParseContentLengthFromHeader(const char* header, std::size_t headerLen,
                                         std::size_t& outLen)
{
    outLen = 0;
    std::size_t i = 0;
    while (i < headerLen)
    {
        std::size_t lineEnd = i;
        while (lineEnd + 1 < headerLen &&
               !(header[lineEnd] == '\r' && header[lineEnd + 1] == '\n'))
            ++lineEnd;
        const std::size_t lineLen = lineEnd > i ? lineEnd - i : 0;
        if (lineLen == 0) break;

        const char* line = header + i;
        if (IStartsWith(std::string_view(line, lineLen), "content-length:"))
        {
            std::size_t k = 15;
            while (k < lineLen && (line[k] == ' ' || line[k] == '\t')) ++k;

            std::size_t value = 0;
            bool any = false;
            while (k < lineLen)
            {
                const char c = line[k];
                if (c < '0' || c > '9') break;
                any = true;
                value = value * 10 + static_cast<std::size_t>(c - '0');
                ++k;
            }
            if (!any) return false;
            outLen = value;
            return true;
        }
        i = lineEnd + 2;
    }
    return true;
}

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
