#include "Stun.h"

#include <cstring>

#if __has_include(<openssl/hmac.h>)
#define STUN_USE_OPENSSL 1
#include <openssl/hmac.h>
#include <openssl/evp.h>
#else
#define STUN_USE_OPENSSL 0
#endif

namespace protocol
{

namespace
{
constexpr uint32_t kFingerprintXor = 0x5354554eu;
constexpr size_t kHeaderSize = 20;
constexpr size_t kAttrHeaderSize = 4;
constexpr size_t kMessageIntegritySize = 20;

static uint16_t EncodeType(StunMethod method, StunClass klass) noexcept
{
    const uint16_t m = static_cast<uint16_t>(method);
    const uint16_t c = static_cast<uint16_t>(klass);

    uint16_t type = 0;
    type |= (m & 0x000F);               // M3..M0
    type |= (m & 0x0070) << 1;          // M6..M4 -> bits 6..4
    type |= (m & 0x0F80) << 2;          // M11..M7 -> bits 11..9

    type |= (c & 0x1) << 4;             // C0 -> bit 4
    type |= ((c >> 1) & 0x1) << 8;      // C1 -> bit 8
    return type;
}

static uint32_t Crc32(const uint8_t* data, size_t len)
{
    uint32_t crc = 0xFFFFFFFFu;
    for (size_t i = 0; i < len; ++i)
    {
        crc ^= data[i];
        for (int j = 0; j < 8; ++j)
        {
            const uint32_t mask = -(crc & 1u);
            crc = (crc >> 1) ^ (0xEDB88320u & mask);
        }
    }
    return ~crc;
}

static bool IsAttrInBounds(const StunMessageInfo& msg, const AttrView& a) noexcept
{
    if (!msg.raw) return false;
    if (a.value_offset > msg.raw_len) return false;
    if (static_cast<size_t>(a.value_offset) + a.len > msg.raw_len) return false;
    return true;
}

static const AttrView* FindLastAttr(const StunMessageInfo& msg, uint16_t t) noexcept
{
    for (auto it = msg.attrs.rbegin(); it != msg.attrs.rend(); ++it)
    {
        if (it->type == t)
            return &(*it);
    }
    return nullptr;
}

} // namespace

bool StunCodec::IsStun(const uint8_t* p, size_t n)
{
    if (!p || n < kHeaderSize)
        return false;

    // First two bits of STUN message are always 0.
    if ((p[0] & 0xC0) != 0)
        return false;

    const uint32_t cookie = ReadBE32(p + 4);
    if (cookie != kMagicCookie)
        return false;

    const uint16_t len = ReadBE16(p + 2);
    if ((len & 0x3) != 0)
        return false;

    if (kHeaderSize + static_cast<size_t>(len) > n)
        return false;

    return true;
}

void StunCodec::DecodeType(uint16_t type_raw, StunMethod& method, StunClass& klass) noexcept
{
    uint16_t m = 0;
    m |= (type_raw & 0x000F);
    m |= (type_raw & 0x00E0) >> 1;
    m |= (type_raw & 0x3E00) >> 2;

    const uint8_t c0 = static_cast<uint8_t>((type_raw >> 4) & 0x1);
    const uint8_t c1 = static_cast<uint8_t>((type_raw >> 8) & 0x1);
    const uint8_t c = static_cast<uint8_t>((c1 << 1) | c0);

    method = static_cast<StunMethod>(m);
    klass = static_cast<StunClass>(c);
}

bool StunCodec::Parse(const uint8_t* p, size_t n, StunMessageInfo& out)
{
    out = {};

    if (!IsStun(p, n))
        return false;

    out.type_raw = ReadBE16(p);
    out.length = ReadBE16(p + 2);
    out.magic_cookie = ReadBE32(p + 4);
    std::memcpy(out.txid.data(), p + 8, out.txid.size());

    DecodeType(out.type_raw, out.method, out.klass);

    out.raw = p;
    out.raw_len = kHeaderSize + out.length;

    size_t off = kHeaderSize;
    const size_t end = kHeaderSize + out.length;

    while (off < end)
    {
        if (off + kAttrHeaderSize > end)
            return false;

        AttrView a;
        a.type = ReadBE16(p + off);
        a.len = ReadBE16(p + off + 2);
        a.value_offset = static_cast<uint32_t>(off + kAttrHeaderSize);

        const size_t padded = Pad4(a.len);
        if (off + kAttrHeaderSize + padded > end)
            return false;

        out.attrs.push_back(a);
        off += kAttrHeaderSize + padded;
    }

    return off == end;
}

bool StunCodec::DecodeUsername(const StunMessageInfo& msg, std::string_view& out)
{
    out = {};
    const AttrView* a = msg.FindAttr(static_cast<uint16_t>(AttrType::USERNAME));
    if (!a || !IsAttrInBounds(msg, *a))
        return false;

    out = msg.AttrValue(*a);
    return !out.empty();
}

bool StunCodec::DecodePriority(const StunMessageInfo& msg, uint32_t& out)
{
    out = 0;
    const AttrView* a = msg.FindAttr(static_cast<uint16_t>(AttrType::PRIORITY));
    if (!a || !IsAttrInBounds(msg, *a) || a->len != 4)
        return false;

    out = ReadBE32(msg.raw + a->value_offset);
    return true;
}

bool StunCodec::DecodeIceControlling(const StunMessageInfo& msg, uint64_t& out)
{
    out = 0;
    const AttrView* a = msg.FindAttr(static_cast<uint16_t>(AttrType::ICE_CONTROLLING));
    if (!a || !IsAttrInBounds(msg, *a) || a->len != 8)
        return false;

    out = ReadBE64(msg.raw + a->value_offset);
    return true;
}

bool StunCodec::DecodeIceControlled(const StunMessageInfo& msg, uint64_t& out)
{
    out = 0;
    const AttrView* a = msg.FindAttr(static_cast<uint16_t>(AttrType::ICE_CONTROLLED));
    if (!a || !IsAttrInBounds(msg, *a) || a->len != 8)
        return false;

    out = ReadBE64(msg.raw + a->value_offset);
    return true;
}

bool StunCodec::DecodeXorMappedAddress(const StunMessageInfo& msg, XorMappedAddress& out)
{
    out = {};

    const AttrView* a = msg.FindAttr(static_cast<uint16_t>(AttrType::XOR_MAPPED_ADDRESS));
    if (!a || !IsAttrInBounds(msg, *a))
        return false;

    const uint8_t* v = msg.raw + a->value_offset;
    if (a->len < 4)
        return false;

    // v[0] reserved
    const uint8_t family = v[1];
    const uint16_t xport = ReadBE16(v + 2);
    out.port = static_cast<uint16_t>(xport ^ static_cast<uint16_t>(kMagicCookie >> 16));

    if (family == 0x01)
    {
        if (a->len != 8)
            return false;

        out.is_ipv6 = false;
        const uint32_t mc = kMagicCookie;
        out.ip[0] = v[4] ^ static_cast<uint8_t>((mc >> 24) & 0xFF);
        out.ip[1] = v[5] ^ static_cast<uint8_t>((mc >> 16) & 0xFF);
        out.ip[2] = v[6] ^ static_cast<uint8_t>((mc >> 8) & 0xFF);
        out.ip[3] = v[7] ^ static_cast<uint8_t>(mc & 0xFF);
        return true;
    }

    if (family == 0x02)
    {
        if (a->len != 20)
            return false;

        out.is_ipv6 = true;

        const uint8_t cookie_bytes[4] = {
            static_cast<uint8_t>((kMagicCookie >> 24) & 0xFF),
            static_cast<uint8_t>((kMagicCookie >> 16) & 0xFF),
            static_cast<uint8_t>((kMagicCookie >> 8) & 0xFF),
            static_cast<uint8_t>(kMagicCookie & 0xFF)
        };

        for (int i = 0; i < 4; ++i)
            out.ip[i] = v[4 + i] ^ cookie_bytes[i];

        for (int i = 0; i < 12; ++i)
            out.ip[4 + i] = v[8 + i] ^ msg.txid[i];

        return true;
    }

    return false;
}

bool StunCodec::HasUseCandidate(const StunMessageInfo& msg) noexcept
{
    return msg.FindAttr(static_cast<uint16_t>(AttrType::USE_CANDIDATE)) != nullptr;
}

bool StunCodec::DecodeErrorCode(const StunMessageInfo& msg, StunErrorCode& out)
{
    out = {};

    const AttrView* a = msg.FindAttr(static_cast<uint16_t>(AttrType::ERROR_CODE));
    if (!a || !IsAttrInBounds(msg, *a) || a->len < 4)
        return false;

    const uint8_t* v = msg.raw + a->value_offset;
    const uint8_t cls = v[2] & 0x07;
    const uint8_t num = v[3];
    out.code = static_cast<uint16_t>(cls * 100 + num);

    if (a->len > 4)
    {
        out.reason.assign(reinterpret_cast<const char*>(v + 4), a->len - 4);
    }
    return true;
}

uint32_t StunCodec::ComputeFingerprint(const uint8_t* data, size_t len)
{
    return Crc32(data, len) ^ kFingerprintXor;
}

bool StunCodec::VerifyFingerprint(const StunMessageInfo& msg)
{
    const AttrView* a = FindLastAttr(msg, static_cast<uint16_t>(AttrType::FINGERPRINT));
    if (!a || !IsAttrInBounds(msg, *a) || a->len != 4)
        return false;

    const size_t attr_start = static_cast<size_t>(a->value_offset) - kAttrHeaderSize;
    if (attr_start + kAttrHeaderSize + 4 > msg.raw_len)
        return false;

    const uint32_t actual = ReadBE32(msg.raw + a->value_offset);
    const uint32_t expect = ComputeFingerprint(msg.raw, attr_start);
    return actual == expect;
}

bool StunCodec::ComputeMessageIntegrity(const uint8_t* data,
                                        size_t len,
                                        std::string_view key,
                                        uint8_t out_hmac[20])
{
    if (!data || !out_hmac)
        return false;

#if STUN_USE_OPENSSL
    unsigned int out_len = 0;
    unsigned char* ret = HMAC(EVP_sha1(),
                              key.data(),
                              static_cast<int>(key.size()),
                              data,
                              len,
                              out_hmac,
                              &out_len);
    return ret != nullptr && out_len == 20;
#else
    (void)data;
    (void)len;
    (void)key;
    std::memset(out_hmac, 0, 20);
    return false;
#endif
}

bool StunCodec::VerifyMessageIntegrity(const StunMessageInfo& msg,
                                       std::string_view key)
{
    const AttrView* a = FindLastAttr(msg, static_cast<uint16_t>(AttrType::MESSAGE_INTEGRITY));
    if (!a || !IsAttrInBounds(msg, *a) || a->len != 20)
        return false;

    const size_t mi_attr_start = static_cast<size_t>(a->value_offset) - kAttrHeaderSize;
    if (mi_attr_start + kAttrHeaderSize + 20 > msg.raw_len)
        return false;

    // Need a temporary copy because STUN HMAC covers a message whose header
    // length stops at MESSAGE-INTEGRITY value end.
    std::vector<uint8_t> tmp(msg.raw, msg.raw + mi_attr_start + kAttrHeaderSize + 20);
    const uint16_t new_len = static_cast<uint16_t>(tmp.size() - kHeaderSize);
    WriteBE16(tmp.data() + 2, new_len);

    uint8_t digest[20] = {0};
    if (!ComputeMessageIntegrity(tmp.data(), tmp.size(), key, digest))
        return false;

    return std::memcmp(digest, msg.raw + a->value_offset, 20) == 0;
}

bool StunCodec::AppendAttr(std::vector<uint8_t>& out,
                           uint16_t type,
                           const void* val,
                           uint16_t len)
{
    WriteBE16(out, type);
    WriteBE16(out, len);

    const auto* p = static_cast<const uint8_t*>(val);
    if (len > 0 && p == nullptr)
        return false;

    for (uint16_t i = 0; i < len; ++i)
        out.push_back(p[i]);

    const size_t pad = Pad4(len) - len;
    for (size_t i = 0; i < pad; ++i)
        out.push_back(0);

    return true;
}

bool StunCodec::AppendUInt32Attr(std::vector<uint8_t>& out,
                                 uint16_t type,
                                 uint32_t val)
{
    uint8_t buf[4];
    WriteBE32(buf, val);
    return AppendAttr(out, type, buf, sizeof(buf));
}

bool StunCodec::AppendUInt64Attr(std::vector<uint8_t>& out,
                                 uint16_t type,
                                 uint64_t val)
{
    uint8_t buf[8];
    WriteBE64(buf, val);
    return AppendAttr(out, type, buf, sizeof(buf));
}

bool StunCodec::AppendUsernameAttr(std::vector<uint8_t>& out,
                                   std::string_view username)
{
    return AppendAttr(out,
                      static_cast<uint16_t>(AttrType::USERNAME),
                      username.data(),
                      static_cast<uint16_t>(username.size()));
}

bool StunCodec::AppendUseCandidateAttr(std::vector<uint8_t>& out)
{
    return AppendAttr(out,
                      static_cast<uint16_t>(AttrType::USE_CANDIDATE),
                      nullptr,
                      0);
}

bool StunCodec::AppendXorMappedAddressAttr(std::vector<uint8_t>& out,
                                           const IpEndpoint& ep,
                                           const std::array<uint8_t, 12>& txid)
{
    uint8_t buf[20] = {0};
    size_t len = 0;

    buf[0] = 0;
    if (ep.family == IpFamily::IPv4)
    {
        buf[1] = 0x01;
        const uint16_t xport = static_cast<uint16_t>(ep.port ^ static_cast<uint16_t>(kMagicCookie >> 16));
        WriteBE16(buf + 2, xport);

        const uint32_t mc = kMagicCookie;
        buf[4] = ep.ip[0] ^ static_cast<uint8_t>((mc >> 24) & 0xFF);
        buf[5] = ep.ip[1] ^ static_cast<uint8_t>((mc >> 16) & 0xFF);
        buf[6] = ep.ip[2] ^ static_cast<uint8_t>((mc >> 8) & 0xFF);
        buf[7] = ep.ip[3] ^ static_cast<uint8_t>(mc & 0xFF);
        len = 8;
    }
    else
    {
        buf[1] = 0x02;
        const uint16_t xport = static_cast<uint16_t>(ep.port ^ static_cast<uint16_t>(kMagicCookie >> 16));
        WriteBE16(buf + 2, xport);

        const uint8_t cookie_bytes[4] = {
            static_cast<uint8_t>((kMagicCookie >> 24) & 0xFF),
            static_cast<uint8_t>((kMagicCookie >> 16) & 0xFF),
            static_cast<uint8_t>((kMagicCookie >> 8) & 0xFF),
            static_cast<uint8_t>(kMagicCookie & 0xFF)
        };

        for (int i = 0; i < 4; ++i)
            buf[4 + i] = ep.ip[i] ^ cookie_bytes[i];

        for (int i = 0; i < 12; ++i)
            buf[8 + i] = ep.ip[4 + i] ^ txid[i];

        len = 20;
    }

    return AppendAttr(out,
                      static_cast<uint16_t>(AttrType::XOR_MAPPED_ADDRESS),
                      buf,
                      static_cast<uint16_t>(len));
}

bool StunCodec::AppendMessageIntegrityAttr(std::vector<uint8_t>& out,
                                           std::string_view key)
{
    // MESSAGE-INTEGRITY must be computed over the message up to and including
    // the MI attribute value, with the header length adjusted accordingly.
    const size_t mi_attr_start = out.size();

    WriteBE16(out, static_cast<uint16_t>(AttrType::MESSAGE_INTEGRITY));
    WriteBE16(out, static_cast<uint16_t>(kMessageIntegritySize));
    out.resize(out.size() + kMessageIntegritySize, 0);

    // Temporarily set message length to include MI.
    if (!UpdateMessageLength(out))
        return false;

    uint8_t digest[20] = {0};
    if (!ComputeMessageIntegrity(out.data(), out.size(), key, digest))
        return false;

    std::memcpy(out.data() + mi_attr_start + kAttrHeaderSize, digest, 20);
    return true;
}

bool StunCodec::AppendFingerprintAttr(std::vector<uint8_t>& out)
{
    const size_t fp_attr_start = out.size();

    WriteBE16(out, static_cast<uint16_t>(AttrType::FINGERPRINT));
    WriteBE16(out, static_cast<uint16_t>(4));
    out.resize(out.size() + 4, 0);

    if (!UpdateMessageLength(out))
        return false;

    const uint32_t fp = ComputeFingerprint(out.data(), fp_attr_start);
    WriteBE32(out.data() + fp_attr_start + kAttrHeaderSize, fp);
    return true;
}

bool StunCodec::UpdateMessageLength(std::vector<uint8_t>& out)
{
    if (out.size() < kHeaderSize)
        return false;

    const size_t len = out.size() - kHeaderSize;
    if (len > 0xFFFF)
        return false;

    WriteBE16(out.data() + 2, static_cast<uint16_t>(len));
    return true;
}

std::vector<uint8_t> StunCodec::BuildBindingRequest(const std::array<uint8_t, 12>& txid)
{
    std::vector<uint8_t> out;
    out.reserve(20);

    WriteBE16(out, EncodeType(StunMethod::Binding, StunClass::Request));
    WriteBE16(out, 0);
    WriteBE32(out, kMagicCookie);
    out.insert(out.end(), txid.begin(), txid.end());
    return out;
}

bool StunCodec::BuildIceBindingRequest(const IceRequestParams& in,
                                       uint8_t* out,
                                       size_t cap,
                                       size_t& out_len)
{
    out_len = 0;

    std::vector<uint8_t> buf = BuildBindingRequest(in.txid);

    if (!AppendUsernameAttr(buf, in.username))
        return false;

    if (!AppendUInt32Attr(buf,
                          static_cast<uint16_t>(AttrType::PRIORITY),
                          in.priority))
        return false;

    if (in.controlling)
    {
        if (!AppendUInt64Attr(buf,
                              static_cast<uint16_t>(AttrType::ICE_CONTROLLING),
                              in.tie_breaker))
            return false;
    }
    else
    {
        if (!AppendUInt64Attr(buf,
                              static_cast<uint16_t>(AttrType::ICE_CONTROLLED),
                              in.tie_breaker))
            return false;
    }

    if (in.use_candidate)
    {
        if (!AppendUseCandidateAttr(buf))
            return false;
    }

    if (!in.password.empty())
    {
        if (!AppendMessageIntegrityAttr(buf, in.password))
            return false;
    }

    if (in.add_fingerprint)
    {
        if (!AppendFingerprintAttr(buf))
            return false;
    }

    if (!UpdateMessageLength(buf))
        return false;

    if (buf.size() > cap)
        return false;

    std::memcpy(out, buf.data(), buf.size());
    out_len = buf.size();
    return true;
}

bool StunCodec::BuildIceBindingSuccess(const IceSuccessParams& in,
                                       uint8_t* out,
                                       size_t cap,
                                       size_t& out_len)
{
    out_len = 0;
    if (!in.req)
        return false;

    std::vector<uint8_t> buf;
    buf.reserve(64);

    WriteBE16(buf, EncodeType(StunMethod::Binding, StunClass::SuccessResponse));
    WriteBE16(buf, 0);
    WriteBE32(buf, kMagicCookie);
    buf.insert(buf.end(), in.req->txid.begin(), in.req->txid.end());

    if (!AppendXorMappedAddressAttr(buf, in.mapped_addr, in.req->txid))
        return false;

    if (!in.password.empty())
    {
        if (!AppendMessageIntegrityAttr(buf, in.password))
            return false;
    }

    if (in.add_fingerprint)
    {
        if (!AppendFingerprintAttr(buf))
            return false;
    }

    if (!UpdateMessageLength(buf))
        return false;

    if (buf.size() > cap)
        return false;

    std::memcpy(out, buf.data(), buf.size());
    out_len = buf.size();
    return true;
}

bool StunCodec::BuildBindingError(const StunMessageInfo& req,
                                  const StunErrorCode& err,
                                  std::string_view password,
                                  uint8_t* out,
                                  size_t cap,
                                  size_t& out_len)
{
    out_len = 0;

    std::vector<uint8_t> buf;
    buf.reserve(128);

    WriteBE16(buf, EncodeType(StunMethod::Binding, StunClass::ErrorResponse));
    WriteBE16(buf, 0);
    WriteBE32(buf, kMagicCookie);
    buf.insert(buf.end(), req.txid.begin(), req.txid.end());

    uint8_t ec[4] = {0, 0, 0, 0};
    const uint16_t cls = static_cast<uint16_t>(err.code / 100);
    const uint16_t num = static_cast<uint16_t>(err.code % 100);
    ec[2] = static_cast<uint8_t>(cls);
    ec[3] = static_cast<uint8_t>(num);

    std::vector<uint8_t> val;
    val.insert(val.end(), ec, ec + 4);
    val.insert(val.end(), err.reason.begin(), err.reason.end());

    if (!AppendAttr(buf,
                    static_cast<uint16_t>(AttrType::ERROR_CODE),
                    val.data(),
                    static_cast<uint16_t>(val.size())))
        return false;

    if (!password.empty())
    {
        if (!AppendMessageIntegrityAttr(buf, password))
            return false;
    }

    if (!AppendFingerprintAttr(buf))
        return false;

    if (!UpdateMessageLength(buf))
        return false;

    if (buf.size() > cap)
        return false;

    std::memcpy(out, buf.data(), buf.size());
    out_len = buf.size();
    return true;
}

} // namespace protocol