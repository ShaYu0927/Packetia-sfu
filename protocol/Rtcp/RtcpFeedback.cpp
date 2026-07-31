#include "RtcpFeedback.h"

#include "RtcpHealper.h"
#include "utils.h"

namespace rtcpx
{
std::vector<uint8_t> RtcpFeedback::BuildPli(uint32_t sender_ssrc, uint32_t media_ssrc)
{
    std::vector<uint8_t> packet(12, 0);
    packet[0] = static_cast<uint8_t>((2U << 6) | 1U); // V=2, FMT=1 (PLI).
    packet[1] = RTCP_PT_PSFB;
    utils::Utils::WriteUint16BE(packet.data() + 2, 2); // Three 32-bit words - 1.
    utils::Utils::WriteUint32BE(packet.data() + 4, sender_ssrc);
    utils::Utils::WriteUint32BE(packet.data() + 8, media_ssrc);
    return packet;
}

bool RtcpFeedback::ParsePli(const uint8_t* data, size_t len, uint32_t* sender_ssrc, uint32_t* media_ssrc)
{
    if (!data || len < 12 || (data[0] >> 6) != 2 || (data[0] & 0x1F) != 1 || data[1] != RTCP_PT_PSFB)
    {
        return false;
    }

    const size_t packet_size = (static_cast<size_t>(utils::Utils::ReadUint16BE(data + 2)) + 1) * 4;
    if (packet_size != 12 || packet_size > len)
    {
        return false;
    }

    if (sender_ssrc)
    {
        *sender_ssrc = utils::Utils::ReadUint32BE(data + 4);
    }
    if (media_ssrc)
    {
        *media_ssrc = utils::Utils::ReadUint32BE(data + 8);
    }
    return true;
}
}
