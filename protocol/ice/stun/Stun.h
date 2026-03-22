#ifndef _STUN_H_
#define _STUN_H_

#include <cstdint>
#include <vector>
#include <array>
#include <cstring>
#include <string>
#include <string_view>

namespace protocol
{

static constexpr uint32_t kMagicCookie = 0x2112A442;

enum class StunClass : uint8_t
{
    Request = 0,
    Indication = 1,
    SuccessResponse = 2,
    ErrorResponse = 3
};

enum class StunMethod : uint16_t
{
    Binding = 0x001,
};

enum class AttrType : uint16_t
{
    MAPPED_ADDRESS      = 0x0001,
    USERNAME            = 0x0006,
    MESSAGE_INTEGRITY   = 0x0008,
    ERROR_CODE          = 0x0009,
    UNKNOWN_ATTRIBUTES  = 0x000A,
    REALM               = 0x0014,
    NONCE               = 0x0015,
    XOR_MAPPED_ADDRESS  = 0x0020,
    PRIORITY            = 0x0024,
    USE_CANDIDATE       = 0x0025,
    SOFTWARE            = 0x8022,
    ALTERNATE_SERVER    = 0x8023,
    FINGERPRINT         = 0x8028,
    ICE_CONTROLLED      = 0x8029,
    ICE_CONTROLLING     = 0x802A,
};

enum class IpFamily : uint8_t
{
    IPv4 = 4,
    IPv6 = 6
};

struct XorMappedAddress
{
    bool is_ipv6 = false;
    uint16_t port = 0;                  // host order
    std::array<uint8_t, 16> ip{};       // IPv4 uses first 4 bytes
};

struct IpEndpoint
{
    IpFamily family = IpFamily::IPv4;
    uint16_t port = 0;                  // host order
    std::array<uint8_t, 16> ip{};       // IPv4 uses first 4 bytes
};

struct AttrView
{
    uint16_t type = 0;
    uint16_t len = 0;
    uint32_t value_offset = 0;          // offset from start of message
};

struct StunMessageInfo
{
    uint16_t type_raw = 0;
    uint16_t length = 0;
    uint32_t magic_cookie = 0;
    std::array<uint8_t, 12> txid{};

    StunMethod method = static_cast<StunMethod>(0);
    StunClass klass = static_cast<StunClass>(0);

    std::vector<AttrView> attrs;

    const uint8_t* raw = nullptr;
    size_t raw_len = 0;

    std::string_view AttrValue(const AttrView& a) const
    {
        if (!raw) return {};
        if ((size_t)a.value_offset + a.len > raw_len) return {};
        return {reinterpret_cast<const char*>(raw + a.value_offset), a.len};
    }

    const AttrView* FindAttr(uint16_t t) const noexcept
    {
        for (const auto& a : attrs)
        {
            if (a.type == t) return &a;
        }
        return nullptr;
    }

    bool HasAttr(uint16_t t) const noexcept
    {
        return FindAttr(t) != nullptr;
    }

    bool IsBindingRequest() const
    {
        return method == StunMethod::Binding && klass == StunClass::Request;
    }

    bool IsBindingResponse() const
    {
        return method == StunMethod::Binding && klass == StunClass::SuccessResponse;
    }

    bool IsBindingErrorResponse() const
    {
        return method == StunMethod::Binding && klass == StunClass::ErrorResponse;
    }
};

struct IceRequestParams
{
    std::array<uint8_t, 12> txid{};

    std::string username;           // remoteUfrag:localUfrag
    uint32_t priority = 0;

    bool controlling = false;
    uint64_t tie_breaker = 0;

    bool use_candidate = false;

    std::string password;           // remote pwd, for MESSAGE-INTEGRITY
    bool add_fingerprint = true;
};

struct IceSuccessParams
{
    const StunMessageInfo* req = nullptr;
    IpEndpoint mapped_addr{};

    std::string password;           // local pwd, for MESSAGE-INTEGRITY
    bool add_fingerprint = true;
};

struct StunErrorCode
{
    uint16_t code = 0;              // e.g. 400 / 401 / 487
    std::string reason;
};

class StunCodec
{
public:
    static bool IsStun(const uint8_t* p, size_t n);
    static bool Parse(const uint8_t* p, size_t n, StunMessageInfo& out);
    static void DecodeType(uint16_t type_raw, StunMethod& method, StunClass& klass) noexcept;

    // ---------- attribute decode helpers ----------
    static bool DecodeUsername(const StunMessageInfo& msg, std::string_view& out);
    static bool DecodePriority(const StunMessageInfo& msg, uint32_t& out);
    static bool DecodeIceControlling(const StunMessageInfo& msg, uint64_t& out);
    static bool DecodeIceControlled(const StunMessageInfo& msg, uint64_t& out);
    static bool DecodeXorMappedAddress(const StunMessageInfo& msg, XorMappedAddress& out);

    static bool HasUseCandidate(const StunMessageInfo& msg) noexcept;
    static bool DecodeErrorCode(const StunMessageInfo& msg, StunErrorCode& out);

    // ---------- integrity / fingerprint ----------
    static uint32_t ComputeFingerprint(const uint8_t* data, size_t len);
    static bool VerifyFingerprint(const StunMessageInfo& msg);

    static bool ComputeMessageIntegrity(const uint8_t* data,
                                        size_t len,
                                        std::string_view key,
                                        uint8_t out_hmac[20]);

    static bool VerifyMessageIntegrity(const StunMessageInfo& msg,
                                       std::string_view key);

    // ---------- build basic stun ----------
    static std::vector<uint8_t> BuildBindingRequest(const std::array<uint8_t, 12>& txid);

    // ---------- build ICE stun ----------
    static bool BuildIceBindingRequest(const IceRequestParams& in,
                                       uint8_t* out,
                                       size_t cap,
                                       size_t& out_len);

    static bool BuildIceBindingSuccess(const IceSuccessParams& in,
                                       uint8_t* out,
                                       size_t cap,
                                       size_t& out_len);

    static bool BuildBindingError(const StunMessageInfo& req,
                                  const StunErrorCode& err,
                                  std::string_view password,
                                  uint8_t* out,
                                  size_t cap,
                                  size_t& out_len);

private:
    static constexpr size_t kHeaderSize = 20;
    static constexpr size_t kTxIdSize = 12;
    static constexpr size_t kMessageIntegritySize = 20;

    static uint16_t ReadBE16(const uint8_t* p) noexcept
    {
        return (uint16_t(p[0]) << 8) | uint16_t(p[1]);
    }

    static uint32_t ReadBE32(const uint8_t* p) noexcept
    {
        return (uint32_t(p[0]) << 24) |
               (uint32_t(p[1]) << 16) |
               (uint32_t(p[2]) << 8)  |
               uint32_t(p[3]);
    }

    static uint64_t ReadBE64(const uint8_t* p) noexcept
    {
        return (uint64_t(p[0]) << 56) |
               (uint64_t(p[1]) << 48) |
               (uint64_t(p[2]) << 40) |
               (uint64_t(p[3]) << 32) |
               (uint64_t(p[4]) << 24) |
               (uint64_t(p[5]) << 16) |
               (uint64_t(p[6]) << 8)  |
               uint64_t(p[7]);
    }

    static void WriteBE16(std::vector<uint8_t>& out, uint16_t v)
    {
        out.push_back(uint8_t((v >> 8) & 0xFF));
        out.push_back(uint8_t(v & 0xFF));
    }

    static void WriteBE32(std::vector<uint8_t>& out, uint32_t v)
    {
        out.push_back(uint8_t((v >> 24) & 0xFF));
        out.push_back(uint8_t((v >> 16) & 0xFF));
        out.push_back(uint8_t((v >> 8) & 0xFF));
        out.push_back(uint8_t(v & 0xFF));
    }

    static void WriteBE64(std::vector<uint8_t>& out, uint64_t v)
    {
        out.push_back(uint8_t((v >> 56) & 0xFF));
        out.push_back(uint8_t((v >> 48) & 0xFF));
        out.push_back(uint8_t((v >> 40) & 0xFF));
        out.push_back(uint8_t((v >> 32) & 0xFF));
        out.push_back(uint8_t((v >> 24) & 0xFF));
        out.push_back(uint8_t((v >> 16) & 0xFF));
        out.push_back(uint8_t((v >> 8) & 0xFF));
        out.push_back(uint8_t(v & 0xFF));
    }

    static inline void WriteBE16(uint8_t* p, uint16_t v)
    {
        p[0] = uint8_t((v >> 8) & 0xFF);
        p[1] = uint8_t(v & 0xFF);
    }

    static inline void WriteBE32(uint8_t* p, uint32_t v)
    {
        p[0] = uint8_t((v >> 24) & 0xFF);
        p[1] = uint8_t((v >> 16) & 0xFF);
        p[2] = uint8_t((v >> 8) & 0xFF);
        p[3] = uint8_t(v & 0xFF);
    }

    static inline void WriteBE64(uint8_t* p, uint64_t v)
    {
        p[0] = uint8_t((v >> 56) & 0xFF);
        p[1] = uint8_t((v >> 48) & 0xFF);
        p[2] = uint8_t((v >> 40) & 0xFF);
        p[3] = uint8_t((v >> 32) & 0xFF);
        p[4] = uint8_t((v >> 24) & 0xFF);
        p[5] = uint8_t((v >> 16) & 0xFF);
        p[6] = uint8_t((v >> 8) & 0xFF);
        p[7] = uint8_t(v & 0xFF);
    }

    static size_t Pad4(size_t x) noexcept
    {
        return (x + 3u) & ~3u;
    }

    static bool AppendAttr(std::vector<uint8_t>& out,
                           uint16_t type,
                           const void* val,
                           uint16_t len);

    static bool AppendUInt32Attr(std::vector<uint8_t>& out,
                                 uint16_t type,
                                 uint32_t val);

    static bool AppendUInt64Attr(std::vector<uint8_t>& out,
                                 uint16_t type,
                                 uint64_t val);

    static bool AppendUsernameAttr(std::vector<uint8_t>& out,
                                   std::string_view username);

    static bool AppendUseCandidateAttr(std::vector<uint8_t>& out);

    static bool AppendXorMappedAddressAttr(std::vector<uint8_t>& out,
                                           const IpEndpoint& ep,
                                           const std::array<uint8_t, 12>& txid);

    static bool AppendMessageIntegrityAttr(std::vector<uint8_t>& out,
                                           std::string_view key);

    static bool AppendFingerprintAttr(std::vector<uint8_t>& out);

    static bool UpdateMessageLength(std::vector<uint8_t>& out);
};

} // namespace protocol

#endif /* _STUN_H_ */