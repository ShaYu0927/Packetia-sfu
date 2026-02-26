#include "Stun.h"

namespace protocol::ice::stun 
{
bool StunCodec::IsStun(const uint8_t*p, size_t n)
{
    if (!p || n < 20) return false;

    if ((p[0] & 0xC0) != 0) return false;

    const auto msg_len = ReadBE16(p + 2);
    const auto cookie  = ReadBE32(p + 4);

    if ((msg_len % 4) != 0) return false;
    if (n < 20u + msg_len) return false;

    return true;
}

std::vector<uint8_t> StunCodec::BuildBindingRequest(uint8_t txid[12])
{
    constexpr std::uint16_t kBindingRequest = 0x0001;

    std::vector<std::uint8_t> out;
    out.reserve(20);

    WriteBE16(out, kBindingRequest);
    WriteBE16(out, 0);               // length = 0 (no attributes)
    WriteBE32(out, kMagicCookie);

    if (txid)
    {
        out.insert(out.end(), txid, txid + 12);
    }
    else
    {
        out.insert(out.end(), txid, txid + 12);
    }
    return out;
}

std::optional<StunMessage> StunCodec::Parse(const uint8_t*p, size_t n)
{
   if (!IsStun(p, n)) return std::nullopt;
   StunMessage m{};
   m.type         = ReadBE16(p + 0);
   m.length       = ReadBE16(p + 2);
   m.magic_cookie = ReadBE32(p + 4);
   std::memcpy(m.txid.data(), p + 8, 12);


   const std::size_t end = 20u + m.length;
   std::size_t off = 20;

    while (off + 4 <= end) 
    {
        const auto atype = ReadBE16(p + off + 0);
        const auto alen  = ReadBE16(p + off + 2);
        off += 4;

        if (off + alen > end) return std::nullopt; 

        StunAttribute a;
        a.type = atype;
        a.value.assign(p + off, p + off + alen);
        m.attrs.emplace_back(std::move(a));

        off += Pad4(alen);
    }

    if (off != end) return std::nullopt;

    return m;
}

}