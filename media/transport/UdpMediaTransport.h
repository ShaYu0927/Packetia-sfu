#ifndef _UDP_MEDIA_TRANSPORT_H_
#define _UDP_MEDIA_TRANSPORT_H_

#include "MediaTransportBase.h"
#include "network/transport/IDatagramTransport.h"

#include <memory>
#include <mutex>

namespace media::transport
{

class UdpMediaTransport final : public MediaTransportBase
{
public:
    UdpMediaTransport(uint64_t id,
                      std::shared_ptr<network::transport::IDatagramTransport> datagram_transport);

    MediaTransportProtocol Protocol() const noexcept override;

    SendResult Send(MediaPacketType type,
                    const uint8_t* data,
                    size_t size,
                    bool retransmit = false) override;

    void Close() override;

    void SetSelectedPeer(const network::SocketAddr& peer);
    bool IsSelectedPeer(const network::SocketAddr& peer) const;

    MediaPacketIngressResult InputDatagram(
        const network::SocketAddr& source,
        MediaPacketType type,
        const uint8_t* data,
        size_t size,
        uint64_t receive_time_ms);

private:
    std::shared_ptr<network::transport::IDatagramTransport> datagram_transport_;
    mutable std::mutex peer_mutex_;
    network::SocketAddr selected_peer_{};
    bool has_selected_peer_{false};
};

} // namespace media::transport

#endif /* _UDP_MEDIA_TRANSPORT_H_ */
