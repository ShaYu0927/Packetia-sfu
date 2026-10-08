#include "TurnAuth.h"
#include "CryptoUtil.h"
#include "StringUtil.h"
#include "TimeUtil.h"

#include <utility>
#include <openssl/crypto.h>
#include <openssl/evp.h>

namespace protocol
{
namespace
{
bool ValidAsciiText(std::string_view text, size_t limit, bool allow_empty = false)
{
    return (allow_empty || !text.empty()) && text.size() <= limit && utils::IsPrintableAscii(text);
}

StunAttribute TextAttribute(AttrType type, std::string_view value)
{
    return {static_cast<uint16_t>(type), {value.begin(), value.end()}};
}

TurnAuthResult InternalError()
{
    TurnAuthResult result;
    result.status = TurnAuthResult::Status::InternalError;
    result.error_code = 500;
    return result;
}

bool SupportedRequest(const StunMessageInfo& msg)
{
    if (msg.klass != StunClass::Request) return false;
    return msg.method == StunMethod::Allocate || msg.method == StunMethod::Refresh ||
        msg.method == StunMethod::CreatePermission || msg.method == StunMethod::ChannelBind;
}
} // namespace

TurnAuth::TurnAuth(std::string realm, PasswordLookup lookup, uint64_t nonce_lifetime_ms,
                   Clock clock, NonceGenerator nonce_generator)
    : realm_(std::move(realm)), lookup_(std::move(lookup)), nonce_lifetime_ms_(nonce_lifetime_ms),
      clock_(std::move(clock)), nonce_generator_(std::move(nonce_generator))
{
    if (!clock_) clock_ = Timestamp::NowMs;
    if (!nonce_generator_)
        nonce_generator_ = [](std::string& nonce) { return utils::SecureRandomHex(32, nonce); };
}

bool TurnAuth::DerivePreparedLegacyKey(std::string_view username, std::string_view realm,
                                      std::string_view password, std::string& out)
{
    out.clear();
    if (username.empty() || realm.empty() || username.size() > 512 || realm.size() > 763 ||
        password.size() > 763 || username.find('\0') != std::string_view::npos ||
        realm.find('\0') != std::string_view::npos || password.find('\0') != std::string_view::npos) return false;

    std::string input;
    input.reserve(username.size() + realm.size() + password.size() + 2);
    input.append(username.data(), username.size());
    input.push_back(':');
    input.append(realm.data(), realm.size());
    input.push_back(':');
    if (!password.empty()) input.append(password.data(), password.size());
    uint8_t digest[EVP_MAX_MD_SIZE];
    unsigned int digest_size = 0;
    const bool ok = EVP_Digest(input.data(), input.size(), digest, &digest_size, EVP_md5(), nullptr) == 1 &&
                    digest_size == 16;
    OPENSSL_cleanse(input.data(), input.size());
    if (ok) out.assign(reinterpret_cast<const char*>(digest), digest_size);
    OPENSSL_cleanse(digest, sizeof(digest));
    return ok;
}

bool TurnAuth::NonceExpired(const TurnAuthContext& context, uint64_t now) const
{
    return context.nonce_.empty() || (now >= context.nonce_issued_ms_ &&
        now - context.nonce_issued_ms_ >= nonce_lifetime_ms_);
}

bool TurnAuth::EnsureNonce(TurnAuthContext& context, uint64_t now) const
{
    if (!NonceExpired(context, now)) return true;
    std::string nonce;
    if (!nonce_generator_(nonce) || !ValidAsciiText(nonce, 763) || nonce == context.nonce_) return false;
    context.nonce_ = std::move(nonce);
    context.nonce_issued_ms_ = now;
    return true;
}

TurnAuthResult TurnAuth::ErrorResponse(const StunMessageInfo& request, uint16_t code,
                                      std::string_view reason, TurnAuthContext& context, uint64_t now) const
{
    std::vector<uint8_t> error = {0, 0, uint8_t(code / 100), uint8_t(code % 100)};
    error.insert(error.end(), reason.begin(), reason.end());
    std::vector<StunAttribute> attributes = {{static_cast<uint16_t>(AttrType::ERROR_CODE), std::move(error)}};
    if (code == 401 || code == 438)
    {
        if (!EnsureNonce(context, now)) return InternalError();
        attributes.push_back(TextAttribute(AttrType::REALM, realm_));
        attributes.push_back(TextAttribute(AttrType::NONCE, context.nonce_));
    }
    TurnAuthResult result;
    if (!StunCodec::BuildMessage(request.method, StunClass::ErrorResponse, request.txid,
                                 attributes, result.response, {}, true)) return InternalError();
    result.status = TurnAuthResult::Status::Response;
    result.error_code = code;
    return result;
}

bool TurnAuth::IsConfigured() const
{
    return ValidAsciiText(realm_, 127) && lookup_ && nonce_lifetime_ms_ != 0;
}

TurnAuthResult TurnAuth::Authenticate(const uint8_t* data, size_t len, TurnAuthContext& context) const
{
    StunMessageInfo msg;
    if (!TurnCodec::ParseStunDatagram(data, len, msg) || !SupportedRequest(msg)) return {};
    if (!IsConfigured()) return InternalError();
    const auto now = clock_();

    size_t fingerprints = 0, integrities = 0;
    for (const auto& attr : msg.attrs)
    {
        if (attr.type == static_cast<uint16_t>(AttrType::FINGERPRINT)) ++fingerprints;
        if (attr.type == static_cast<uint16_t>(AttrType::MESSAGE_INTEGRITY)) ++integrities;
    }
    // FINGERPRINT is optional for TURN. Corruption is silently discarded.
    if (fingerprints && (fingerprints != 1 || !StunCodec::VerifyFingerprint(msg))) return {};
    if (integrities > 1 || msg.HasAttr(static_cast<uint16_t>(AttrType::MESSAGE_INTEGRITY_SHA256)))
        return ErrorResponse(msg, 400, "Unsupported integrity attributes", context, now);
    const auto* integrity = msg.FindAttr(static_cast<uint16_t>(AttrType::MESSAGE_INTEGRITY));
    if (!integrity) return ErrorResponse(msg, 401, "Unauthorized", context, now);
    if (integrity->len != 20) return ErrorResponse(msg, 400, "Bad MESSAGE-INTEGRITY", context, now);

    // Only attributes before MESSAGE-INTEGRITY can supply authenticated values.
    // Reject duplicate credentials rather than let different layers pick different copies.
    size_t usernames = 0, realms = 0, nonces = 0;
    for (const auto& attr : msg.attrs)
    {
        if (attr.type == static_cast<uint16_t>(AttrType::MESSAGE_INTEGRITY)) break;
        if (attr.type == static_cast<uint16_t>(AttrType::USERNAME)) ++usernames;
        if (attr.type == static_cast<uint16_t>(AttrType::REALM)) ++realms;
        if (attr.type == static_cast<uint16_t>(AttrType::NONCE)) ++nonces;
    }
    if (usernames != 1 || realms != 1 || nonces != 1)
        return ErrorResponse(msg, 400, "Missing or duplicate credentials", context, now);

    const auto username = msg.AttrValue(*msg.FindAttr(static_cast<uint16_t>(AttrType::USERNAME)));
    const auto realm = msg.AttrValue(*msg.FindAttr(static_cast<uint16_t>(AttrType::REALM)));
    const auto nonce = msg.AttrValue(*msg.FindAttr(static_cast<uint16_t>(AttrType::NONCE)));
    if (!ValidAsciiText(username, 512) || !ValidAsciiText(realm, 127) || !ValidAsciiText(nonce, 763))
        return ErrorResponse(msg, 400, "Bad credential encoding", context, now);
    if (realm != realm_) return ErrorResponse(msg, 401, "Unauthorized", context, now);

    // Like coturn's per-session nonce, expiry or mismatch triggers a fresh
    // challenge before credential lookup. No relay allocation is authorized.
    if (NonceExpired(context, now) || nonce.size() != context.nonce_.size() ||
        CRYPTO_memcmp(nonce.data(), context.nonce_.data(), nonce.size()) != 0)
        return ErrorResponse(msg, 438, "Stale Nonce", context, now);

    std::string password;
    if (!lookup_(username, password)) return ErrorResponse(msg, 401, "Unauthorized", context, now);
    std::string key;
    const bool derived = ValidAsciiText(password, 763, true) &&
        DerivePreparedLegacyKey(username, realm_, password, key);
    OPENSSL_cleanse(password.data(), password.size());
    if (!derived) return InternalError();
    if (!StunCodec::VerifyMessageIntegrity(msg, key))
    {
        OPENSSL_cleanse(key.data(), key.size());
        return ErrorResponse(msg, 401, "Unauthorized", context, now);
    }
    TurnAuthResult result;
    result.status = TurnAuthResult::Status::Authorized;
    result.username.assign(username.data(), username.size());
    result.integrity_key = std::move(key);
    return result;
}

} // namespace protocol
