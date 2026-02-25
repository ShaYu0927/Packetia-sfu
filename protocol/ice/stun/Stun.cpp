#include "Stun.h"

namespace protocol::ice::stun 
{
bool StunCodec::IsStun(const uint8_t*, size_t)
{

}

std::vector<uint8_t> StunCodec::BuildBindingRequest(uint8_t txid[12])
{

}

std::optional<StunMessage> StunCodec::Parse(const uint8_t*, size_t)
{

}

}