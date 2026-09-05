#ifndef _NETWORK_TRANSPORT_DATAGRAM_PROTOCOL_CLASSIFIER_H_
#define _NETWORK_TRANSPORT_DATAGRAM_PROTOCOL_CLASSIFIER_H_

#include <cstddef>
#include <cstdint>

namespace network::transport
{

enum class DatagramProtocol
{
    Stun = 0,
    Dtls,
    Rtp,
    Rtcp,
    Unknown
};

/* RFC 7983 first-byte demultiplexing with basic packet-shape validation. */
class DatagramProtocolClassifier
{
public:
    static DatagramProtocol Classify(const uint8_t* data, size_t size) noexcept;

    static bool IsStun(const uint8_t* data, size_t size) noexcept;
    static bool IsDtls(const uint8_t* data, size_t size) noexcept;
    static bool IsRtp(const uint8_t* data, size_t size) noexcept;
    static bool IsRtcp(const uint8_t* data, size_t size) noexcept;
};

} // namespace network::transport

#endif /* _NETWORK_TRANSPORT_DATAGRAM_PROTOCOL_CLASSIFIER_H_ */
