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

bool StunCodec::BuildBindingSuccess(const StunMessageInfo& req,
                             const IpEndpoint& src,
                             uint8_t* out, size_t cap,
                             size_t& out_len)
{
    out_len = 0;
    if (!out || cap < 20 + 4 + 8) return false;
    if (src.family != 4) return false;

    constexpr uint16_t kBindingSuccess = 0x0101;
    constexpr uint16_t kAttrXorMapped  = 0x0020;
    constexpr uint32_t kMagicCookie    = 0x2112A442;

    uint8_t* p = out;
    WriteBE16(p + 0, kBindingSuccess);
    WriteBE16(p + 2, 0);              
    WriteBE32(p + 4, kMagicCookie);
    std::memcpy(p + 8, req.txid.data(), 12);

    size_t off = 20;

    WriteBE16(p + off + 0, kAttrXorMapped);
    WriteBE16(p + off + 2, 8); 
    off += 4;

    p[off + 0] = 0; 
    p[off + 1] = 0x01;

    const uint16_t xport = uint16_t(src.port ^ uint16_t(kMagicCookie >> 16));
    WriteBE16(p + off + 2, xport);

    uint32_t ip_be = (uint32_t(src.ip[0]) << 24) |
                     (uint32_t(src.ip[1]) << 16) |
                     (uint32_t(src.ip[2]) << 8)  |
                     (uint32_t(src.ip[3]) << 0);

    const uint32_t xip = ip_be ^ kMagicCookie;
    WriteBE32(p + off + 4, xip);

    off += 8;
    const uint16_t body_len = uint16_t(off - 20);
    WriteBE16(p + 2, body_len);

    out_len = off;
    return true;

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