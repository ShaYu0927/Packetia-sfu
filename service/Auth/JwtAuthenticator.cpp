#include "JwtAuthenticator.h"

#include "third/nlohmann/json.hpp"
#include <openssl/crypto.h>
#include <openssl/evp.h>
#include <openssl/hmac.h>
#include <chrono>
#include <limits>
#include <set>
#include <stdexcept>
#include <vector>

namespace service::auth
{
namespace
{
using Json = nlohmann::json;
constexpr size_t kMaxTokenBytes = 8192;
constexpr size_t kMaxHeaderBytes = 1024;
constexpr size_t kSha256Bytes = 32;
constexpr size_t kMinSecretBytes = 32;
constexpr size_t kMaxSecretBytes = 4096;
constexpr int kMaxJsonDepth = 8;

int64_t UnixSeconds()
{
    return std::chrono::duration_cast<std::chrono::seconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();
}

bool DecodeBase64Url(std::string_view encoded, std::string& decoded)
{
    if (encoded.empty() || encoded.size() % 4 == 1) return false;
    decoded.clear();
    decoded.reserve(encoded.size() * 3 / 4);
    unsigned bits = 0;
    unsigned accumulator = 0;
    for (const unsigned char c : encoded)
    {
        unsigned value;
        if (c >= 'A' && c <= 'Z') value = c - 'A';
        else if (c >= 'a' && c <= 'z') value = c - 'a' + 26;
        else if (c >= '0' && c <= '9') value = c - '0' + 52;
        else if (c == '-') value = 62;
        else if (c == '_') value = 63;
        else return false; // JWT uses URL-safe, unpadded base64, without whitespace.
        accumulator = (accumulator << 6) | value;
        bits += 6;
        if (bits >= 8)
        {
            bits -= 8;
            decoded.push_back(static_cast<char>((accumulator >> bits) & 0xff));
        }
    }
    // Reject alternate encodings whose unused trailing bits are nonzero.
    return bits == 0 || (accumulator & ((1u << bits) - 1u)) == 0;
}

Json ParseObject(const std::string& text)
{
    std::vector<std::set<std::string>> object_keys;
    auto callback = [&](int depth, Json::parse_event_t event, Json& parsed)
    {
        if (depth > kMaxJsonDepth) throw std::runtime_error("JWT JSON depth");
        if (event == Json::parse_event_t::object_start) object_keys.emplace_back();
        else if (event == Json::parse_event_t::key)
        {
            if (object_keys.empty() || !object_keys.back().insert(parsed.get<std::string>()).second)
                throw std::runtime_error("Duplicate JWT claim");
        }
        else if (event == Json::parse_event_t::object_end) object_keys.pop_back();
        return true;
    };
    auto value = Json::parse(text, callback);
    if (!value.is_object()) throw std::runtime_error("JWT object expected");
    return value;
}

bool ReadString(const Json& object, const char* key, std::string& output,
                bool required, size_t max_size = 256)
{
    const auto found = object.find(key);
    if (found == object.end()) return !required;
    if (!found->is_string()) return false;
    output = found->get<std::string>();
    if ((required && output.empty()) || output.size() > max_size) return false;
    for (const unsigned char c : output)
        if (c < 0x20 || c == 0x7f) return false;
    return true;
}

bool ReadTimestamp(const Json& object, const char* key, int64_t& output, bool required)
{
    const auto found = object.find(key);
    if (found == object.end()) return !required;
    if (found->is_number_unsigned())
    {
        const auto number = found->get<uint64_t>();
        if (number > static_cast<uint64_t>(std::numeric_limits<int64_t>::max())) return false;
        output = static_cast<int64_t>(number);
    }
    else if (found->is_number_integer()) output = found->get<int64_t>();
    else return false;
    return output >= 0;
}

bool ReadPermission(const Json& object, const char* key, bool& output)
{
    const auto found = object.find(key);
    if (found == object.end()) return true;
    if (!found->is_boolean()) return false;
    output = found->get<bool>();
    return true;
}

bool HasAudience(const Json& claims, std::string_view expected)
{
    const auto found = claims.find("aud");
    if (found == claims.end()) return false;
    if (found->is_string()) return found->get<std::string>() == expected;
    if (!found->is_array() || found->empty() || found->size() > 16) return false;
    bool matched = false;
    for (const auto& item : *found)
    {
        if (!item.is_string() || item.get_ref<const std::string&>().empty()) return false;
        if (item.get_ref<const std::string&>() == expected) matched = true;
    }
    return matched;
}
}

JwtAuthenticator::JwtAuthenticator(std::shared_ptr<const IKeyProvider> keys, Clock clock)
    : keys_(std::move(keys)), clock_(clock ? std::move(clock) : Clock{UnixSeconds})
{
}

AuthResult JwtAuthenticator::Authenticate(std::string_view token, Audience expected_audience) const
{
    if (!keys_) return AuthResult::Failure(AuthError::NotConfigured);
    if (token.empty()) return AuthResult::Failure(AuthError::Unauthenticated);
    if (token.size() > kMaxTokenBytes) return AuthResult::Failure(AuthError::InvalidToken);
    const char* audience;
    switch (expected_audience)
    {
    case Audience::WebRtc: audience = "packetia-webrtc"; break;
    case Audience::Control: audience = "packetia-control"; break;
    default: return AuthResult::Failure(AuthError::WrongAudience);
    }

    try
    {
        const auto first = token.find('.');
        if (first == std::string_view::npos) return AuthResult::Failure(AuthError::InvalidToken);
        const auto second = token.find('.', first + 1);
        if (second == std::string_view::npos || token.find('.', second + 1) != std::string_view::npos)
            return AuthResult::Failure(AuthError::InvalidToken);
        std::string header_text, claims_text, signature;
        if (!DecodeBase64Url(token.substr(0, first), header_text) || header_text.size() > kMaxHeaderBytes ||
            !DecodeBase64Url(token.substr(first + 1, second - first - 1), claims_text) ||
            !DecodeBase64Url(token.substr(second + 1), signature) || signature.size() != kSha256Bytes)
            return AuthResult::Failure(AuthError::InvalidToken);

        const auto header = ParseObject(header_text);
        std::string algorithm, type;
        if (!ReadString(header, "alg", algorithm, true) || algorithm != "HS256" ||
            !ReadString(header, "typ", type, false) || (header.contains("typ") && type != "JWT") ||
            header.contains("crit") || header.contains("b64"))
            return AuthResult::Failure(AuthError::InvalidToken);

        const auto claims = ParseObject(claims_text);
        AuthContext context;
        if (!ReadString(claims, "iss", context.issuer, true))
            return AuthResult::Failure(AuthError::InvalidToken);
        // Unverified iss is only a lookup key. No claims become trusted before HMAC validation.
        const auto secret = keys_->SecretFor(context.issuer);
        if (!secret || secret->size() < kMinSecretBytes || secret->size() > kMaxSecretBytes)
            return AuthResult::Failure(AuthError::InvalidToken);
        unsigned char digest[EVP_MAX_MD_SIZE];
        unsigned digest_size = 0;
        if (!HMAC(EVP_sha256(), secret->data(), static_cast<int>(secret->size()),
                  reinterpret_cast<const unsigned char*>(token.data()), second, digest, &digest_size))
            return AuthResult::Failure(AuthError::InternalError);
        if (digest_size != kSha256Bytes || CRYPTO_memcmp(digest, signature.data(), kSha256Bytes) != 0)
            return AuthResult::Failure(AuthError::InvalidToken);

        if (!HasAudience(claims, audience)) return AuthResult::Failure(AuthError::WrongAudience);
        if (!ReadString(claims, "sub", context.subject_id, true) ||
            !ReadString(claims, "jti", context.token_id, false) ||
            !ReadString(claims, "name", context.name, false) ||
            !ReadTimestamp(claims, "exp", context.expires_at_unix_seconds, true) ||
            context.expires_at_unix_seconds == 0)
            return AuthResult::Failure(AuthError::InvalidToken);

        int64_t not_before = 0, issued_at = 0;
        if (!ReadTimestamp(claims, "nbf", not_before, false) ||
            !ReadTimestamp(claims, "iat", issued_at, false) ||
            not_before >= context.expires_at_unix_seconds || issued_at >= context.expires_at_unix_seconds)
            return AuthResult::Failure(AuthError::InvalidToken);
        const auto now = clock_();
        if (now >= context.expires_at_unix_seconds) return AuthResult::Failure(AuthError::ExpiredToken);
        if (now < not_before || now < issued_at) return AuthResult::Failure(AuthError::InvalidToken);

        const auto video = claims.find("video");
        // Source-restricted publishing is not implemented. Do not silently widen such a grant.
        if (video != claims.end() && (!video->is_object() || video->contains("canPublishSources") ||
            !ReadString(*video, "room", context.video.room, false) ||
            !ReadPermission(*video, "roomJoin", context.video.room_join) ||
            !ReadPermission(*video, "canPublish", context.video.can_publish) ||
            !ReadPermission(*video, "canSubscribe", context.video.can_subscribe)))
            return AuthResult::Failure(AuthError::InvalidToken);
        context.audience = expected_audience;
        return AuthResult::Success(std::move(context));
    }
    catch (const std::exception&)
    {
        return AuthResult::Failure(AuthError::InvalidToken);
    }
}

}
