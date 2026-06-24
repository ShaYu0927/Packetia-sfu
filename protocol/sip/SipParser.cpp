#include "SipParser.h"

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <string>

namespace sip
{
namespace
{
int FindCrlfCrlf(const char* p, size_t n)
{
    for (size_t i = 0; i + 3 < n; ++i)
    {
        if (p[i] == '\r' && p[i + 1] == '\n' && p[i + 2] == '\r' && p[i + 3] == '\n')
        {
            return static_cast<int>(i);
        }
    }
    return -1;
}

bool StartsWithNoCase(const char* s, size_t n, const char* prefix)
{
    size_t m = 0;
    while (prefix[m]) ++m;
    if (n < m) return false;

    for (size_t i = 0; i < m; ++i)
    {
        unsigned char a = static_cast<unsigned char>(s[i]);
        unsigned char b = static_cast<unsigned char>(prefix[i]);
        if (std::tolower(a) != std::tolower(b)) return false;
    }
    return true;
}

bool ParseContentLength(const char* header, size_t header_len, size_t& out_len)
{
    out_len = 0;
    size_t i = 0;
    while (i < header_len)
    {
        size_t line_end = i;
        while (line_end + 1 < header_len && !(header[line_end] == '\r' && header[line_end + 1] == '\n'))
        {
            ++line_end;
        }

        size_t line_len = line_end > i ? line_end - i : 0;
        if (line_len == 0) break;

        const char* line = header + i;
        if (StartsWithNoCase(line, line_len, "content-length:"))
        {
            size_t k = 15;
            while (k < line_len && (line[k] == ' ' || line[k] == '\t')) ++k;
            char* end = nullptr;
            unsigned long len = std::strtoul(line + k, &end, 10);
            if (end == line + k) return false;
            out_len = static_cast<size_t>(len);
            return true;
        }

        i = line_end + 2;
    }

    return true;
}
} // namespace

protocol::ProtocolParser::ParseResult SipParser::Parse(BufferReader& buffer)
{
    const size_t readable = buffer.ReadableBytes();
    if (readable < 4) return ParseResult::NeedMoreData;

    const char* p = buffer.Peek();
    int header_end = FindCrlfCrlf(p, readable);
    if (header_end < 0)
    {
        constexpr size_t kMaxHeader = 32 * 1024;
        return readable > kMaxHeader ? ParseResult::Error : ParseResult::NeedMoreData;
    }

    size_t header_len = static_cast<size_t>(header_end) + 4;
    size_t body_len = 0;
    if (!ParseContentLength(p, header_len, body_len)) return ParseResult::Error;

    constexpr size_t kMaxBody = 2 * 1024 * 1024;
    if (body_len > kMaxBody) return ParseResult::Error;

    return readable >= header_len + body_len ? ParseResult::Ok : ParseResult::NeedMoreData;
}

} // namespace sip