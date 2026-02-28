#include "Stun.h"

#include <cstdint>
#include <vector>
#include <array>
#include <cstring>


namespace protocol
{
bool StunCodec::IsStun(const uint8_t*p, size_t n)
{
    if (!p || n < 20) return false;

    if ((p[0] & 0xC0) != 0) return false;

    const auto msg_len = ReadBE16(p + 2);
    const auto cookie  = ReadBE32(p + 4);

    if ((msg_len % 4) != 0) return false;
    if (n < 20u + msg_len) return false;
    if (cookie != kMagicCookie) return false;

    return true;
}

std::vector<uint8_t> StunCodec::BuildBindingRequest(uint8_t txid[12])
{
    constexpr std::uint16_t kBindingRequest = 0x0001;

    std::vector<std::uint8_t> out;
    out.reserve(20);

    WriteBE16(out, kBindingRequest);
    WriteBE16(out, 0);              
    WriteBE32(out, kMagicCookie);

    if (txid)
    {
        out.insert(out.end(), txid, txid + 12);
    }
    else
    {
        out.insert(out.end(), 12, std::uint8_t{0});
    }
    return out;
}

void StunCodec::DecodeType(uint16_t type_raw, StunMethod& method, StunClass& klass) noexcept
{
    const uint16_t cls =
        uint16_t(((type_raw & 0x0100) >> 7) | ((type_raw & 0x0010) >> 4));

    const uint16_t m =
        uint16_t((type_raw & 0x000F) |
                 ((type_raw & 0x00E0) >> 1) |
                 ((type_raw & 0x3E00) >> 2));

    method = static_cast<StunMethod>(m);
    klass = static_cast<StunClass>(cls);
}

bool StunCodec::Parse(const uint8_t*p, size_t n, StunMessageInfo& m)
{
    if (!IsStun(p, n)) return false;
    m = {};
    m.raw = p;
    m.raw_len = n;

    m.type_raw     = ReadBE16(p + 0);
    m.length       = ReadBE16(p + 2);
    m.magic_cookie = ReadBE32(p + 4);
    std::memcpy(m.txid.data(), p + 8, 12);

    if (m.magic_cookie != kMagicCookie) return false;
    if (n < 20u) return false;

    const std::size_t end = 20u + m.length;
    if (end > n) return false;


    DecodeType(m.type_raw, m.method, m.klass);

    std::size_t off = 20u;
    while (off + 4 <= end) 
    {
        const uint16_t atype = ReadBE16(p + off + 0);
        const uint16_t alen  = ReadBE16(p + off + 2);
        off += 4u;

        if (off + alen > end) return false; 

        AttrView av{};
        av.type = atype;
        av.len  = alen;
        av.value_offset = static_cast<uint32_t>(off);
        m.attrs.emplace_back(av);

        off += alen;
        off = Pad4(off);
    }

    if (off != end) return false;
    return true;
}

}