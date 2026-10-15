#ifndef PACKETIA_SERVICE_ACCOUNT_POLICY_H_
#define PACKETIA_SERVICE_ACCOUNT_POLICY_H_

#include "AccountTypes.h"
#include <string_view>

namespace service::account
{

// ASCII identifiers are case-insensitive. Only surrounding ASCII spaces are
// stripped; Unicode display names remain independent of login identifiers.
inline bool NormalizeLogin(std::string_view input, std::string& output)
{
    if (input.size() > 256) return false;
    while (!input.empty() && input.front() == ' ') input.remove_prefix(1);
    while (!input.empty() && input.back() == ' ') input.remove_suffix(1);
    if (input.size() < 3) return false;
    output.clear();
    for (char ch : input)
    {
        if (ch >= 'A' && ch <= 'Z') ch += 'a' - 'A';
        if (!((ch >= 'a' && ch <= 'z') || (ch >= '0' && ch <= '9') ||
              ch == '.' || ch == '_' || ch == '-' || ch == '@' || ch == '+')) return false;
        output.push_back(ch);
    }
    return true;
}

inline bool ValidDisplayName(std::string_view text)
{
    if (text.size() > 256) return false;
    // Validate UTF-8 as well as controls, including direct non-HTTP callers.
    for (size_t i = 0; i < text.size();)
    {
        const auto first = static_cast<unsigned char>(text[i++]);
        if (first < 0x20 || first == 0x7f) return false;
        if (first < 0x80) continue;
        unsigned remaining, codepoint, minimum;
        if (first >= 0xc2 && first <= 0xdf) { remaining = 1; codepoint = first & 0x1f; minimum = 0x80; }
        else if (first >= 0xe0 && first <= 0xef) { remaining = 2; codepoint = first & 0x0f; minimum = 0x800; }
        else if (first >= 0xf0 && first <= 0xf4) { remaining = 3; codepoint = first & 7; minimum = 0x10000; }
        else return false;
        if (remaining > text.size() - i) return false;
        while (remaining--)
        {
            const auto next = static_cast<unsigned char>(text[i++]);
            if ((next & 0xc0) != 0x80) return false;
            codepoint = (codepoint << 6) | (next & 0x3f);
        }
        if (codepoint < minimum || codepoint > 0x10ffff ||
            (codepoint >= 0xd800 && codepoint <= 0xdfff) ||
            (codepoint >= 0x80 && codepoint <= 0x9f)) return false;
    }
    return true;
}

inline bool ValidPasswordInput(std::string_view password)
{
    return !password.empty() && password.size() <= 4096 &&
           password.find('\0') == std::string_view::npos;
}

inline AccountError SessionValidity(const StoredSession& snapshot, int64_t now)
{
    if (snapshot.user.status != UserStatus::Active) return AccountError::UserDisabled;
    if (snapshot.session.revoked_at_unix_seconds != 0 ||
        snapshot.session.credential_version != snapshot.user.credential_version)
        return AccountError::SessionRevoked;
    if (now >= snapshot.session.expires_at_unix_seconds) return AccountError::SessionExpired;
    if (now <= 0 || snapshot.session.created_at_unix_seconds > now ||
        snapshot.session.user_id != snapshot.user.id) return AccountError::Unauthenticated;
    return AccountError::None;
}

}
#endif
