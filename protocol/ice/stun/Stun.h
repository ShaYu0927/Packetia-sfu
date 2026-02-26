#ifndef _STUN_H_
#define _STUN_H_

#include <cstdint>
#include <string>
#include <vector>
#include <optional>
#include <array>
#include <memory>
#include <cstring>

namespace protocol::ice::stun 
{
/* rfc8489 */    
static constexpr uint32_t kMagicCookie = 0x2112A442;

enum class MsgType : uint16_t 
{
    BindingRequest  = 0x0001,
    BindingSuccess  = 0x0101,
    BindingError    = 0x0111,
};

typedef struct XorMappedAddress
{
    bool is_ipv6 = false;
    std::string ip;
    uint16_t port = 0;
}XorMappedAddress;

typedef struct StunAttribute 
{
  std::uint16_t type{};
  std::vector<std::uint8_t> value; 
}StunAttribute;

typedef struct StunMessage 
{
  std::uint16_t type{};
  std::uint16_t length{}; 
  std::uint32_t magic_cookie{};
  std::array<std::uint8_t, 12> txid{};
  std::vector<StunAttribute> attrs;

  const StunAttribute* FindAttr(std::uint16_t t) const noexcept 
  {
    for (auto& a : attrs) if (a.type == t) return &a;
    return nullptr;
  }
}StunMessage;

class StunCodec 
{
public:
    static bool IsStun(const uint8_t*, size_t);
    static std::vector<uint8_t> BuildBindingRequest(uint8_t txid[12]);
    static std::optional<StunMessage> Parse(const uint8_t*, size_t);

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

    static std::size_t Pad4(std::size_t x) noexcept { return (x + 3u) & ~3u; }
};

}


#endif /* _STUN_H_ */