#ifndef _STUN_H_
#define _STUN_H_

#include <cstdint>
#include <string>
#include <vector>
#include <optional>

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

typedef struct StunMessage 
{
    uint16_t msg_type = 0;
    uint16_t msg_len = 0;
    uint32_t magic_cookie = 0;
    uint8_t transaction_id[12] = {0};

    std::optional<XorMappedAddress> xor_mapped_addr;
}StunMessage;

class StunCodec 
{
public:
  static bool IsStun(const uint8_t*, size_t);
  static std::vector<uint8_t> BuildBindingRequest(uint8_t txid[12]);
  static std::optional<StunMessage> Parse(const uint8_t*, size_t);
};

}


#endif /* _STUN_H_ */