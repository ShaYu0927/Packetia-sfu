#include "RtspDigestAuthenticator.h"
#include <openssl/crypto.h>
#include <openssl/evp.h>
#include <openssl/rand.h>
#include <algorithm>
#include <charconv>
#include <chrono>
#include <cctype>

namespace service::auth
{
namespace
{
int64_t UnixSeconds()
{
    return std::chrono::duration_cast<std::chrono::seconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();
}
std::string Lower(std::string value)
{
    for (auto& ch : value) ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
    return value;
}
std::string Hex(const unsigned char* data, std::size_t size)
{
    constexpr char digits[] = "0123456789abcdef";
    std::string result;
    result.reserve(size * 2);
    for (std::size_t i = 0; i < size; ++i) { result += digits[data[i] >> 4]; result += digits[data[i] & 15]; }
    return result;
}
std::string Md5(const std::string& text)
{
    unsigned char digest[EVP_MAX_MD_SIZE];
    unsigned int size = 0;
    if (EVP_Digest(text.data(), text.size(), digest, &size, EVP_md5(), nullptr) != 1 || size != 16) return {};
    return Hex(digest, size);
}
bool Parse(std::string_view header, std::map<std::string, std::string>& fields)
{
    if (header.size() > 8192) return false;
    for (unsigned char ch : header) if (ch < 32 && ch != '\t') return false;
    const auto space = header.find_first_of(" \t");
    if (space == header.npos || Lower(std::string(header.substr(0, space))) != "digest") return false;
    std::size_t pos = space;
    const auto skip = [&] { while (pos < header.size() && (header[pos] == ' ' || header[pos] == '\t')) ++pos; };
    while (pos < header.size())
    {
        skip();
        const auto begin = pos;
        while (pos < header.size() && ((header[pos] >= 'a' && header[pos] <= 'z') ||
               (header[pos] >= 'A' && header[pos] <= 'Z') || header[pos] == '_')) ++pos;
        if (pos == begin) return false;
        auto key = Lower(std::string(header.substr(begin, pos - begin)));
        skip();
        if (pos == header.size() || header[pos++] != '=') return false;
        skip();
        std::string value;
        if (pos < header.size() && header[pos] == '"')
        {
            ++pos;
            bool closed = false;
            while (pos < header.size())
            {
                char ch = header[pos++];
                if (ch == '"') { closed = true; break; }
                if (ch == '\\') { if (pos == header.size()) return false; ch = header[pos++]; }
                value += ch;
            }
            if (!closed) return false;
        }
        else
        {
            const auto start = pos;
            while (pos < header.size() && header[pos] != ',' && header[pos] != ' ' && header[pos] != '\t') ++pos;
            value = std::string(header.substr(start, pos - start));
        }
        if (value.empty() || fields.size() >= 16 || !fields.emplace(std::move(key), std::move(value)).second) return false;
        skip();
        if (pos == header.size()) break;
        if (header[pos++] != ',') return false;
        skip();
        if (pos == header.size()) return false;
    }
    return !fields.empty();
}
}

StaticRtspCredentialProvider::StaticRtspCredentialProvider(const RtspAuthOptions& options)
{
    options.Validate();
    for (const auto& user : options.users) users_.emplace(user.username, user);
}
std::optional<RtspCredential> StaticRtspCredentialProvider::Find(std::string_view username) const
{
    const auto found = users_.find(username);
    return found == users_.end() ? std::nullopt : std::optional<RtspCredential>(found->second);
}

RtspDigestAuthenticator::RtspDigestAuthenticator(std::shared_ptr<const IRtspCredentialProvider> credentials,
    std::string realm, uint32_t nonce_ttl_seconds, bool allow_legacy, Clock clock)
    : credentials_(std::move(credentials)), realm_(std::move(realm)), ttl_(nonce_ttl_seconds),
      allow_legacy_(allow_legacy), clock_(clock ? std::move(clock) : Clock{UnixSeconds})
{
    RtspAuthOptions options;
    options.realm = realm_; options.nonce_ttl_seconds = ttl_; options.Validate();
}

std::string RtspDigestAuthenticator::Challenge(bool stale) const
{
    const auto now = clock_();
    if (nonce_.empty() || stale || legacy_used_ || now < issued_at_ || now - issued_at_ >= ttl_)
    {
        unsigned char bytes[32];
        if (RAND_bytes(bytes, sizeof(bytes)) != 1) throw std::runtime_error("RTSP nonce generation failed");
        nonce_ = Hex(bytes, sizeof(bytes));
        issued_at_ = now; last_nc_ = 0; legacy_used_ = false;
    }
    return "Digest realm=\"" + realm_ + "\", nonce=\"" + nonce_ +
        "\", algorithm=MD5, qop=\"auth\"" + (stale ? ", stale=true" : "");
}

AuthResult RtspDigestAuthenticator::Authenticate(std::string_view, Audience) const
{
    // 禁止通过只含 token 的接口绕过方法和 URI 校验。
    return AuthResult::Failure(AuthError::InvalidToken);
}

AuthResult RtspDigestAuthenticator::Authenticate(const AuthRequest& request, Audience audience) const
{
    if (audience != Audience::Rtsp) return AuthResult::Failure(AuthError::WrongAudience);
    if (!credentials_) return AuthResult::Failure(AuthError::NotConfigured);
    std::map<std::string, std::string> fields;
    const auto deny = [] { return AuthResult::Failure(AuthError::InvalidToken); };
    if (request.method.empty() || request.uri.empty() || !Parse(request.credential, fields)) return deny();
    for (const auto* name : {"username", "realm", "nonce", "uri", "response"})
        if (!fields.count(name)) return deny();
    if (fields["realm"] != realm_ || fields["uri"] != request.uri || nonce_.empty() || fields["nonce"] != nonce_ ||
        (fields.count("algorithm") && Lower(fields["algorithm"]) != "md5") || fields.count("userhash")) return deny();
    const auto now = clock_();
    if (now < issued_at_ || now - issued_at_ >= ttl_ || legacy_used_)
        return AuthResult::Failure(AuthError::ExpiredToken);
    uint32_t nc = 0;
    const bool qop = fields.count("qop") != 0;
    if (qop)
    {
        if (fields["qop"] != "auth" || !fields.count("cnonce") || fields["cnonce"].size() > 256 ||
            !fields.count("nc") || fields["nc"].size() != 8) return deny();
        const auto& text = fields["nc"];
        const auto parsed = std::from_chars(text.data(), text.data() + text.size(), nc, 16);
        if (parsed.ec != std::errc{} || parsed.ptr != text.data() + text.size() || nc <= last_nc_) return deny();
    }
    else if (!allow_legacy_ || fields.count("cnonce") || fields.count("nc") || last_nc_) return deny();
    const auto user = credentials_->Find(fields["username"]);
    if (!user || user->username != fields["username"] || user->ha1.size() != 32) return deny();
    const auto ha2 = Md5(std::string(request.method) + ":" + std::string(request.uri));
    if (ha2.empty()) return AuthResult::Failure(AuthError::NotConfigured);
    auto input = user->ha1 + ":" + nonce_ + ":";
    if (qop) input += fields["nc"] + ":" + fields["cnonce"] + ":auth:";
    const auto expected = Md5(input + ha2);
    const auto response = Lower(fields["response"]);
    if (expected.empty() || response.size() != expected.size() ||
        CRYPTO_memcmp(expected.data(), response.data(), expected.size()) != 0) return deny();
    // 仅校验成功后推进重放状态，错误请求不能消耗正确客户端的 nc。
    if (qop) last_nc_ = nc; else legacy_used_ = true;
    AuthContext context;
    context.subject_id = user->username; context.issuer = realm_; context.audience = Audience::Rtsp;
    context.expires_at_unix_seconds = issued_at_ + ttl_;
    context.streams = user->streams;
    return AuthResult::Success(std::move(context));
}
}
