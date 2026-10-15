#include "JoinToken.h"
#include "third/nlohmann/json.hpp"
#include <openssl/evp.h>
#include <openssl/hmac.h>
#include <stdexcept>

namespace service::auth
{
namespace
{
std::string Encode(const unsigned char* data, std::size_t size)
{
    std::string result(4 * ((size + 2) / 3), '\0');
    result.resize(EVP_EncodeBlock(reinterpret_cast<unsigned char*>(result.data()), data, static_cast<int>(size)));
    while (!result.empty() && result.back() == '=') result.pop_back();
    for (auto& c : result) { if (c == '+') c = '-'; else if (c == '/') c = '_'; }
    return result;
}
std::string Encode(const std::string& value)
{
    return Encode(reinterpret_cast<const unsigned char*>(value.data()), value.size());
}
}
std::string SignJoinToken(const AuthContext& c, const std::string& secret, int64_t issued_at)
{
    if (secret.size() < 32 || secret.size() > 4096 || c.issuer.empty() || c.subject_id.empty() ||
        c.video.room.empty() || !c.video.room_join || c.node_id.empty() || c.instance_id.empty() ||
        c.expires_at_unix_seconds <= issued_at || c.audience != Audience::WebRtc)
        throw std::invalid_argument("invalid join token configuration");
    using Json = nlohmann::json;
    const auto claims = Json{{"iss", c.issuer}, {"sub", c.subject_id}, {"name", c.name},
        {"aud", "packetia-webrtc"}, {"iat", issued_at}, {"exp", c.expires_at_unix_seconds},
        {"jti", c.token_id}, {"node_id", c.node_id}, {"instance_id", c.instance_id},
        {"video", {{"room", c.video.room}, {"roomJoin", true},
                   {"canPublish", c.video.can_publish}, {"canSubscribe", c.video.can_subscribe}}}};
    const auto content = Encode(Json{{"alg", "HS256"}, {"typ", "JWT"}}.dump()) + "." + Encode(claims.dump());
    unsigned char digest[EVP_MAX_MD_SIZE];
    unsigned size = 0;
    if (!HMAC(EVP_sha256(), secret.data(), static_cast<int>(secret.size()),
        reinterpret_cast<const unsigned char*>(content.data()), content.size(), digest, &size))
        throw std::runtime_error("join token signing failed");
    return content + "." + Encode(digest, size);
}
}
