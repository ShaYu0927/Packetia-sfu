#ifndef PACKETIA_TURN_AUTH_H
#define PACKETIA_TURN_AUTH_H

#include "TurnCodec.h"

#include <functional>
#include <string>

namespace protocol
{

// One context per client/server 5-tuple, owned by the future TURN session.
// All operations must run on that session's event loop.
class TurnAuthContext
{
    friend class TurnAuth;
    std::string nonce_;
    uint64_t nonce_issued_ms_ = 0;
};

struct TurnAuthResult
{
    enum class Status { Ignored, Authorized, Response, InternalError };
    Status status = Status::Ignored;
    uint16_t error_code = 0;
    std::vector<uint8_t> response;
    std::string username;
    // Raw 16-byte key for signing the eventual method response. Never log it.
    std::string integrity_key;
};

class TurnAuth
{
public:
    using Clock = std::function<uint64_t()>; // Monotonic milliseconds.
    using PasswordLookup = std::function<bool(std::string_view username, std::string& password)>;
    using NonceGenerator = std::function<bool(std::string& nonce)>;

    TurnAuth(std::string realm, PasswordLookup lookup, uint64_t nonce_lifetime_ms = 600000,
             Clock clock = {}, NonceGenerator nonce_generator = {});

    // Legacy long-term credentials, with printable ASCII accounts only.
    // Authorizes Allocate/Refresh/CreatePermission/ChannelBind requests;
    // does not allocate resources or authorize Send/ChannelData traffic.
    TurnAuthResult Authenticate(const uint8_t* data, size_t len, TurnAuthContext& context) const;
    bool IsConfigured() const;

    // Caller has already applied the credential preparation required by the
    // selected STUN version. This function performs no Unicode normalization.
    static bool DerivePreparedLegacyKey(std::string_view username, std::string_view realm,
                                        std::string_view password, std::string& out);

private:
    bool EnsureNonce(TurnAuthContext& context, uint64_t now) const;
    bool NonceExpired(const TurnAuthContext& context, uint64_t now) const;
    TurnAuthResult ErrorResponse(const StunMessageInfo& request, uint16_t code,
                                 std::string_view reason, TurnAuthContext& context, uint64_t now) const;

    std::string realm_;
    PasswordLookup lookup_;
    uint64_t nonce_lifetime_ms_;
    Clock clock_;
    NonceGenerator nonce_generator_;
};

} // namespace protocol

#endif
