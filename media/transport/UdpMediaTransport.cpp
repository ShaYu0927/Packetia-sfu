#include "UdpMediaTransport.h"

#include <utility>

namespace media::transport
{

UdpMediaTransport::UdpMediaTransport(
    uint64_t id,
    std::shared_ptr<network::transport::IDatagramTransport> datagram_transport)
    : MediaTransportBase(id),
      datagram_transport_(std::move(datagram_transport))
{
    SetState(!datagram_transport_ || !datagram_transport_->IsWritable()
        ? MediaTransportState::Failed
        : MediaTransportState::Connecting);
}

MediaTransportProtocol UdpMediaTransport::Protocol() const noexcept
{
    return MediaTransportProtocol::Udp;
}

SendResult UdpMediaTransport::Send(MediaPacketType type,
                                   const uint8_t* data,
                                   size_t size,
                                   bool retransmit)
{
    (void)type;
    (void)retransmit;
    if (State() != MediaTransportState::Connected)
    {
        return IsClosed() ? SendResult::Closed : SendResult::NotWritable;
    }
    if (!data || size == 0)
    {
        return SendResult::Failed;
    }

    network::SocketAddr peer;
    std::shared_ptr<network::transport::IDatagramTransport> datagram_transport;
    {
        std::lock_guard<std::mutex> lock(peer_mutex_);
        datagram_transport = datagram_transport_;
        if (!has_selected_peer_)
        {
            return SendResult::NotWritable;
        }
        peer = selected_peer_;
    }
    if (!datagram_transport || !datagram_transport->IsWritable())
    {
        SetState(MediaTransportState::Closed);
        return SendResult::Closed;
    }

    switch (datagram_transport->SendDatagram(peer, data, size))
    {
    case network::transport::DatagramSendResult::Ok:
        return SendResult::Ok;
    case network::transport::DatagramSendResult::Closed:
        SetState(MediaTransportState::Closed);
        return SendResult::Closed;
    case network::transport::DatagramSendResult::NotWritable:
        return SendResult::NotWritable;
    case network::transport::DatagramSendResult::Failed:
    default:
        return SendResult::Failed;
    }
}

void UdpMediaTransport::Close()
{
    const auto state = State();
    if (state == MediaTransportState::Closed)
    {
        return;
    }
    SetState(MediaTransportState::Closing);
    DetachPacketSink();
    {
        std::lock_guard<std::mutex> lock(peer_mutex_);
        has_selected_peer_ = false;
        selected_peer_ = {};
    }
    datagram_transport_.reset();
    SetState(MediaTransportState::Closed);
}

void UdpMediaTransport::SetSelectedPeer(const network::SocketAddr& peer)
{
    if (peer.len == 0 || IsClosed())
    {
        return;
    }
    {
        std::lock_guard<std::mutex> lock(peer_mutex_);
        if (!datagram_transport_ || !datagram_transport_->IsWritable())
        {
            return;
        }
        selected_peer_ = peer;
        has_selected_peer_ = true;
    }
    SetState(MediaTransportState::Connected);
}

bool UdpMediaTransport::IsSelectedPeer(const network::SocketAddr& peer) const
{
    std::lock_guard<std::mutex> lock(peer_mutex_);
    return has_selected_peer_ && selected_peer_ == peer;
}

MediaPacketIngressResult UdpMediaTransport::InputDatagram(
    const network::SocketAddr& source,
    MediaPacketType type,
    const uint8_t* data,
    size_t size,
    uint64_t receive_time_ms)
{
    if (!IsSelectedPeer(source))
    {
        return MediaPacketIngressResult::Dropped;
    }
    return PublishPacket(type, data, size, receive_time_ms);
}

} // namespace media::transport
