#ifndef _NETWORK_TRANSPORT_DATAGRAM_PACKET_H_
#define _NETWORK_TRANSPORT_DATAGRAM_PACKET_H_

#include "UdpSocket.h"

#include <cstddef>
#include <cstdint>
#include <utility>
#include <vector>

namespace network::transport
{

/*
 * One UDP datagram as observed by the network layer.
 *
 * No protocol classification is carried here. STUN, DTLS, RTP and RTCP are
 * consumers' concerns. The payload owns its storage so it may cross an I/O
 * thread boundary safely.
 */
struct ReceivedDatagram
{
    uint64_t transport_id = 0;
    uint64_t receive_time_ms = 0;
    network::SocketAddr remote{};
    std::vector<uint8_t> payload;

    ReceivedDatagram() = default;

    ReceivedDatagram(uint64_t id,
                     uint64_t received_at_ms,
                     const network::SocketAddr& source,
                     const uint8_t* data,
                     size_t size)
        : transport_id(id),
          receive_time_ms(received_at_ms),
          remote(source)
    {
        if (data && size > 0)
        {
            payload.assign(data, data + size);
        }
    }

    bool IsValid() const noexcept
    {
        return transport_id != 0 && remote.len != 0 && !payload.empty();
    }

    const uint8_t* Data() const noexcept { return payload.data(); }
    size_t Size() const noexcept { return payload.size(); }
};

} // namespace network::transport

#endif /* _NETWORK_TRANSPORT_DATAGRAM_PACKET_H_ */
