#pragma once

#include "AuthTypes.h"
#include <algorithm>
#include <set>
#include <stdexcept>
#include <string_view>

namespace service::auth
{
// 与 RTSP 媒体索引使用相同的流路径；不做 URL 解码或通配符匹配。
inline bool IsValidStreamPath(std::string_view path)
{
    if (path.empty() || path.size() > 256 || path.front() == '/' || path.back() == '/') return false;
    for (const unsigned char ch : path)
        if (!((ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z') ||
              (ch >= '0' && ch <= '9') || ch == '_' || ch == '-' || ch == '.' || ch == '/')) return false;
    std::size_t begin = 0;
    while (begin < path.size())
    {
        const auto end = path.find('/', begin);
        const auto part = path.substr(begin, end == path.npos ? end : end - begin);
        if (part.empty() || part == "." || part == "..") return false;
        if (end == path.npos) break;
        begin = end + 1;
    }
    return true;
}

struct RtspCredential
{
    std::string username;
    std::string ha1;
    std::vector<StreamGrant> streams;
};

struct RtspAuthOptions
{
    bool enabled = false;
    std::string realm = "packetia";
    uint32_t nonce_ttl_seconds = 300;
    bool allow_legacy_digest = true;
    std::vector<RtspCredential> users;

    void Validate() const
    {
        const auto safe = [](std::string_view value) {
            return !value.empty() && value.size() <= 128 &&
                std::all_of(value.begin(), value.end(), [](unsigned char ch) {
                    return ch >= 33 && ch <= 126 && ch != '"' && ch != '\\' && ch != ':';
                });
        };
        if (!safe(realm) || !nonce_ttl_seconds || nonce_ttl_seconds > 3600 || users.size() > 256 ||
            (enabled && users.empty())) throw std::invalid_argument("invalid RTSP auth options or missing users");
        std::set<std::string> names;
        for (const auto& user : users)
        {
            if (!safe(user.username) || !names.insert(user.username).second || user.ha1.size() != 32 ||
                !std::all_of(user.ha1.begin(), user.ha1.end(), [](unsigned char ch) {
                    return (ch >= '0' && ch <= '9') || (ch >= 'a' && ch <= 'f');
                }) || user.streams.empty() || user.streams.size() > 128)
                throw std::invalid_argument("invalid RTSP username, HA1 or grants");
            std::set<std::string> paths;
            for (const auto& grant : user.streams)
                if (!IsValidStreamPath(grant.path) || !paths.insert(grant.path).second ||
                    (!grant.can_publish && !grant.can_play)) throw std::invalid_argument("invalid RTSP stream grant");
        }
    }
};
}
