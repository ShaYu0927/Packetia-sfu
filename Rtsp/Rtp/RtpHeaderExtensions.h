#ifndef PACKETIA_RTP_HEADER_EXTENSIONS_H_
#define PACKETIA_RTP_HEADER_EXTENSIONS_H_

#include <cstddef>
#include <cstdint>
#include <string>
#include <unordered_map>

namespace rtsp
{
/**
 * RTP extension envelope: RFC 3550 section 5.3.1.
 * It follows the 12-byte fixed header and CC * 4 bytes of CSRC identifiers.
 * X=1 means exactly one envelope, which can contain multiple RFC 8285 elements.
 * All multi-byte fields use network byte order (most significant byte first).
 *
 *  0                   1                   2                   3
 *  0 1 2 3 4 5 6 7 8 9 0 1 2 3 4 5 6 7 8 9 0 1 2 3 4 5 6 7 8 9 0 1
 * +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
 * |       defined by profile      |       length (32-bit words)   |
 * +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
 * |                  extension data (length * 4 bytes)            |
 * |                              ...                              |
 * +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
 *
 * Length excludes the 4-byte envelope and includes extension alignment padding.
 * Zero words is valid. Extension alignment padding is distinct from RTP P-bit
 * trailing padding; extension data must not extend into payload or RTP padding.
 *
 * One-byte elements: RFC 8285 section 4.2, profile = 0xBEDE.
 * +-+-+-+-+-+-+-+-+-------------------------------+
 * |  ID   |  len  |       data (len + 1 bytes)      |
 * +-+-+-+-+-+-+-+-+-------------------------------+
 * ID 1..14; data length 1..16. 0x00 is a single padding byte.
 * ID 15 terminates element parsing, regardless of its length bits.
 * RFC 8285 section 4.1.2 also requires termination for ID 0 with nonzero len;
 * this helper does not yet implement that special case.
 *
 * Two-byte elements: RFC 8285 section 4.3, profile = 0x1000 | appbits.
 * +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-------------------------------+
 * |       ID      |     length    |       data (length bytes)      |
 * +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-------------------------------+
 * ID 1..255; data length 0..255 (no +1). ID 0 is one padding byte,
 * without a length byte. The low four profile bits are application-defined.
 *
 * IDs identify SDP a=extmap URIs, not fixed global extension types. Negotiating
 * format mixing (a=extmap-allow-mixed) belongs to signaling, not this parser.
 *
 * Helper contract: packet is plaintext RTP, excluding SRTP trailers. Unknown
 * profiles are skipped by their envelope length. Duplicate element IDs are
 * rejected by local policy. Values are binary strings, not necessarily text.
 * Pass an empty values map. On failure it may contain entries already parsed;
 * callers must discard those entries. See ../../README.md (RTP) for support.
 */
inline bool ReadRtpHeaderExtensions(const uint8_t* packet, size_t size,
                                    std::unordered_map<uint8_t, std::string>& values)
{
    if (!packet || size < 12 || (packet[0] >> 6) != 2) return false;
    size_t offset = 12 + 4 * (packet[0] & 15);
    if (offset > size) return false;
    if (packet[0] & 0x10)
    {
        if (offset + 4 > size) return false;
        const uint16_t profile = (uint16_t(packet[offset]) << 8) | packet[offset + 1];
        const size_t end = offset + 4 + 4 * ((uint16_t(packet[offset + 2]) << 8) | packet[offset + 3]);
        offset += 4;
        if (end > size) return false;
        if (profile == 0xBEDE || (profile & 0xFFF0) == 0x1000)
        {
            while (offset < end)
            {
                uint8_t id = packet[offset++];
                if (id == 0) continue;
                size_t length;
                if (profile == 0xBEDE)
                {
                    length = (id & 15) + 1;
                    id >>= 4;
                    if (id == 15) break;
                }
                else
                {
                    if (offset == end) return false;
                    length = packet[offset++];
                }
                if (offset + length > end || values.count(id)) return false;
                values.emplace(id, std::string(reinterpret_cast<const char*>(packet + offset), length));
                offset += length;
            }
        }
        offset = end;
    }
    if (packet[0] & 0x20)
    {
        const size_t padding = packet[size - 1];
        if (!padding || padding > size - offset) return false;
    }
    return true;
}
}

#endif // PACKETIA_RTP_HEADER_EXTENSIONS_H_
