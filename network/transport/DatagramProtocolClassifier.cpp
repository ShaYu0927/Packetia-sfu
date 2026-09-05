#include "DatagramProtocolClassifier.h"

namespace network::transport
{

bool DatagramProtocolClassifier::IsStun(const uint8_t* data,
                                        size_t size) noexcept
{
    if (!data || size < 20 || (data[0] & 0xC0) != 0)
    {
        return false;
    }
    const uint32_t cookie = (uint32_t(data[4]) << 24) |
                            (uint32_t(data[5]) << 16) |
                            (uint32_t(data[6]) << 8) |
                            uint32_t(data[7]);
    return cookie == 0x2112A442;
}

bool DatagramProtocolClassifier::IsDtls(const uint8_t* data,
                                        size_t size) noexcept
{
    if (!data || size < 13 || data[0] < 20 || data[0] > 23)
    {
        return false;
    }
    return data[1] == 0xFE && (data[2] == 0xFD || data[2] == 0xFF);
}

bool DatagramProtocolClassifier::IsRtcp(const uint8_t* data,
                                        size_t size) noexcept
{
    return data && size >= 4 && (data[0] >> 6) == 2 &&
           data[1] >= 192 && data[1] <= 223;
}

bool DatagramProtocolClassifier::IsRtp(const uint8_t* data,
                                       size_t size) noexcept
{
    return data && size >= 12 && (data[0] >> 6) == 2 &&
           !IsRtcp(data, size);
}

DatagramProtocol DatagramProtocolClassifier::Classify(
    const uint8_t* data,
    size_t size) noexcept
{
    if (IsStun(data, size)) return DatagramProtocol::Stun;
    if (IsDtls(data, size)) return DatagramProtocol::Dtls;
    if (IsRtcp(data, size)) return DatagramProtocol::Rtcp;
    if (IsRtp(data, size)) return DatagramProtocol::Rtp;
    return DatagramProtocol::Unknown;
}

} // namespace network::transport
