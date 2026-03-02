#ifndef _STUN_H_
#define _STUN_H_

#include <cstdint>
#include <vector>
#include <array>
#include <cstring>
#include <string_view>

namespace protocol
{
static constexpr uint32_t kMagicCookie = 0x2112A442;

enum class StunClass : uint8_t
{
    Request = 0, Indication = 1, SuccessResponse = 2, ErrorResponse = 3
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
    SOFTWARE            = 0x8022,
    ALTERNATE_SERVER    = 0x8023,
    FINGERPRINT         = 0x8028,
    PRIORITY            = 0x0024,
    USE_CANDIDATE       = 0x0025,
    ICE_CONTROLLED      = 0x8029,
    ICE_CONTROLLING     = 0x802A,
};

struct XorMappedAddress 
{
    bool is_ipv6 = false;
    uint16_t port = 0;
    std::array<uint8_t, 16> ip{}; // v4 
};

struct AttrView
{
    uint16_t type = 0;
    uint16_t len = 0;
    uint32_t value_offset = 0; // offset from start of message
};

struct IpEndpoint
{
    uint8_t family;                 // 4 or 6
    uint16_t port;                  // host order
    std::array<uint8_t, 16> ip{};   // ip bytes; ipv4 use first 4
};
    
typedef struct StunMessageInfo
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
        return {(const char*)raw + a.value_offset, a.len};
    }

    const AttrView* FindAttr(uint16_t t) const noexcept
    {
        for (const auto& a : attrs) if (a.type == t) return &a;
        return nullptr;
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
}StunMessageInfo;

class StunCodec 
{
public:
    static bool IsStun(const uint8_t*, size_t);
    static std::vector<uint8_t> BuildBindingRequest(uint8_t txid[12]);
    static bool Parse(const uint8_t* p, size_t n, StunMessageInfo& out);
    static void DecodeType(uint16_t type_raw, StunMethod& method, StunClass& klass) noexcept;
    static bool BuildBindingSuccess(const StunMessageInfo& req,
                             const IpEndpoint& src,
                             uint8_t* out, size_t cap,
                             size_t& out_len);


private:
    static constexpr std::uint32_t kMagicCookie = 0x2112A442;

    static std::uint16_t ReadBE16(const std::uint8_t* p) noexcept 
    {
       return (std::uint16_t(p[0]) << 8) | std::uint16_t(p[1]);
    }

    static std::uint32_t ReadBE32(const std::uint8_t* p) noexcept 
    {
        return (std::uint32_t(p[0]) << 24) | (std::uint32_t(p[1]) << 16) |
            (std::uint32_t(p[2]) << 8)  |  std::uint32_t(p[3]);
    }

    static void WriteBE16(std::vector<std::uint8_t>& out, std::uint16_t v) 
    {
        out.push_back(std::uint8_t((v >> 8) & 0xFF));
        out.push_back(std::uint8_t(v & 0xFF));
    }

    static void WriteBE32(std::vector<std::uint8_t>& out, std::uint32_t v) 
    {
        out.push_back(std::uint8_t((v >> 24) & 0xFF));
        out.push_back(std::uint8_t((v >> 16) & 0xFF));
        out.push_back(std::uint8_t((v >> 8) & 0xFF));
        out.push_back(std::uint8_t(v & 0xFF));
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

    static std::size_t Pad4(std::size_t x) noexcept { return (x + 3u) & ~3u; }
};

}

#endif /* _STUN_H_ */