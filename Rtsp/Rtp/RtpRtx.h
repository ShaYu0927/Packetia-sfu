#ifndef PACKETIA_RTP_RTX_H
#define PACKETIA_RTP_RTX_H

#include <cstddef>
#include <cstdint>

namespace rtsp
{
// Plaintext RFC 3550 packet bounds, excluding RTP padding. SRTP must already
// have been removed. Extension and CSRC bytes are part of the preserved header.
inline bool RtpPayloadBounds(const uint8_t* data, size_t size, size_t& begin, size_t& end)
{
    if (!data || size < 12 || data[0] >> 6 != 2) return false;
    begin = 12 + 4 * (data[0] & 15);
    if (begin > size) return false;
    if (data[0] & 0x10) {
        if (size - begin < 4) return false;
        begin += 4 + 4 * ((size_t(data[begin + 2]) << 8) | data[begin + 3]);
        if (begin > size) return false;
    }
    end = size;
    if (data[0] & 0x20) {
        const size_t padding = data[size - 1];
        if (!padding || padding > size - begin) return false;
        end -= padding;
    }
    return true;
}

// RFC 4588: independent RTX SSRC/sequence/PT, original sequence number (OSN)
// prepended to the original payload. Timestamp and marker are preserved.
// Templates accept std::vector and the sender's pool-allocated ByteVector.
template<class Bytes>
bool EncapsulateRtx(Bytes& packet, uint8_t pt, uint32_t ssrc, uint16_t sequence)
{
    size_t begin, end;
    if (pt > 127 || !ssrc || !RtpPayloadBounds(packet.data(), packet.size(), begin, end) || begin == end) return false;
    const uint8_t osn[] = {packet[2], packet[3]};
    packet.resize(end);
    packet.insert(packet.begin() + begin, osn, osn + 2);
    packet[0] &= ~0x20;
    packet[1] = (packet[1] & 0x80) | pt;
    packet[2] = static_cast<uint8_t>(sequence >> 8);
    packet[3] = static_cast<uint8_t>(sequence);
    for (int i = 0; i < 4; ++i) packet[8 + i] = static_cast<uint8_t>(ssrc >> (24 - 8 * i));
    return true;
}

template<class Bytes>
bool DecapsulateRtx(Bytes& packet, uint8_t primary_pt, uint32_t primary_ssrc)
{
    size_t begin, end;
    if (primary_pt > 127 || !primary_ssrc || !RtpPayloadBounds(packet.data(), packet.size(), begin, end) ||
        end - begin <= 2) return false;
    const uint8_t high = packet[begin], low = packet[begin + 1];
    packet.resize(end);
    packet.erase(packet.begin() + begin, packet.begin() + begin + 2);
    packet[0] &= ~0x20;
    packet[1] = (packet[1] & 0x80) | primary_pt;
    packet[2] = high;
    packet[3] = low;
    for (int i = 0; i < 4; ++i) packet[8 + i] = static_cast<uint8_t>(primary_ssrc >> (24 - 8 * i));
    return true;
}
}
#endif
