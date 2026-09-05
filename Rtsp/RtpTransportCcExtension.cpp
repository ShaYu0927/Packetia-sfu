#include "RtpTransportCcExtension.h"

namespace rtsp
{

bool RtpTransportCcExtension::Read(const uint8_t* packet,
                                   size_t size,
                                   uint8_t extension_id,
                                   uint16_t* transport_sequence)
{
    if (!packet || !transport_sequence || size < 12 || extension_id == 0 || extension_id > 14)
    {
        return false;
    }
    if ((packet[0] >> 6) != 2 || (packet[0] & 0x10) == 0)
    {
        return false;
    }

    const size_t base_header_size = 12 + static_cast<size_t>(packet[0] & 0x0F) * 4;
    if (size < base_header_size + 4)
    {
        return false;
    }
    const uint16_t profile = static_cast<uint16_t>(packet[base_header_size] << 8) |
                             packet[base_header_size + 1];
    if (profile != 0xBEDE)
    {
        return false;
    }

    const uint16_t words = static_cast<uint16_t>(packet[base_header_size + 2] << 8) |
                           packet[base_header_size + 3];
    const size_t begin = base_header_size + 4;
    const size_t end = begin + static_cast<size_t>(words) * 4;
    if (size < end)
    {
        return false;
    }

    for (size_t offset = begin; offset < end;)
    {
        const uint8_t header = packet[offset];
        if (header == 0)
        {
            ++offset;
            continue;
        }
        const uint8_t id = header >> 4;
        if (id == 15)
        {
            break;
        }
        const size_t value_size = static_cast<size_t>(header & 0x0F) + 1;
        if (offset + 1 + value_size > end)
        {
            return false;
        }
        if (id == extension_id)
        {
            if (value_size != 2)
            {
                return false;
            }
            *transport_sequence = static_cast<uint16_t>(packet[offset + 1] << 8) |
                                  packet[offset + 2];
            return true;
        }
        offset += 1 + value_size;
    }
    return false;
}

}
