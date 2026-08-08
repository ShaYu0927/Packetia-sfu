#include "UdpMediaTransport.h"

#include <utility>

namespace media::transport
{

UdpMediaTransport::UdpMediaTransport(
    uint64_t id,
    std::weak_ptr<network::UdpServer> server)
    : MediaTransportBase(id),
      server_(std::move(server))
{
    SetState(server_.expired()
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

    auto server = server_.lock();
    if (!server)
    {
        SetState(MediaTransportState::Closed);
        return SendResult::Closed;
    }

    network::SocketAddr peer;
    {
        std::lock_guard<std::mutex> lock(peer_mutex_);
        if (!has_selected_peer_)
        {
            return SendResult::NotWritable;
        }
        peer = selected_peer_;
    }

    return server->SendTo(peer, data, size)
        ? SendResult::Ok
        : SendResult::Failed;
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
    server_.reset();
    SetState(MediaTransportState::Closed);
}

void UdpMediaTransport::SetSelectedPeer(const network::SocketAddr& peer)
{
    if (peer.len == 0 || server_.expired() || IsClosed())
    {
        return;
    }
    {
        std::lock_guard<std::mutex> lock(peer_mutex_);
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
