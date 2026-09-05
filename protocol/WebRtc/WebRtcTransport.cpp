#include "WebRtcTransport.h"

#include <utility>

namespace protocol::webrtc
{

WebRtcTransport::WebRtcTransport(
    uint64_t id,
    std::shared_ptr<network::transport::IDatagramTransport> datagram_transport)
    : id_(id), datagram_transport_(std::move(datagram_transport))
{
    if (id_ == 0 || !datagram_transport_)
    {
        state_.store(WebRtcTransportState::Failed, std::memory_order_release);
    }
}

WebRtcTransportState WebRtcTransport::State() const noexcept
{
    return state_.load(std::memory_order_acquire);
}

bool WebRtcTransport::Start()
{
    WebRtcTransportState expected = WebRtcTransportState::Created;
    if (!state_.compare_exchange_strong(expected,
                                        WebRtcTransportState::Connecting,
                                        std::memory_order_acq_rel))
    {
        return expected == WebRtcTransportState::Connecting ||
               expected == WebRtcTransportState::Connected;
    }
    if (!datagram_transport_ || !datagram_transport_->IsWritable())
    {
        state_.store(WebRtcTransportState::Failed, std::memory_order_release);
        return false;
    }
    auto self = weak_from_this();
    if (self.expired())
    {
        state_.store(WebRtcTransportState::Failed, std::memory_order_release);
        return false;
    }
    datagram_transport_->SetDatagramSink(self);
    return true;
}

void WebRtcTransport::Close()
{
    const auto state = State();
    if (state == WebRtcTransportState::Closed ||
        state == WebRtcTransportState::Closing)
    {
        return;
    }
    state_.store(WebRtcTransportState::Closing, std::memory_order_release);

    std::shared_ptr<IWebRtcTransportSink> sink;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        sink = sink_.lock();
        sink_.reset();
        selected_peer_ = {};
        has_selected_peer_ = false;
    }
    if (datagram_transport_)
    {
        datagram_transport_->SetDatagramSink({});
    }
    state_.store(WebRtcTransportState::Closed, std::memory_order_release);
    if (sink)
    {
        sink->OnWebRtcTransportClosed();
    }
}

void WebRtcTransport::SetSink(std::weak_ptr<IWebRtcTransportSink> sink)
{
    std::lock_guard<std::mutex> lock(mutex_);
    sink_ = std::move(sink);
}

bool WebRtcTransport::SelectPeer(const network::SocketAddr& peer)
{
    const auto state = State();
    if (peer.len == 0 ||
        (state != WebRtcTransportState::Connecting &&
         state != WebRtcTransportState::Connected))
    {
        return false;
    }
    {
        std::lock_guard<std::mutex> lock(mutex_);
        selected_peer_ = peer;
        has_selected_peer_ = true;
    }
    state_.store(WebRtcTransportState::Connected, std::memory_order_release);
    return true;
}

bool WebRtcTransport::IsSelectedPeer(const network::SocketAddr& peer) const
{
    std::lock_guard<std::mutex> lock(mutex_);
    return has_selected_peer_ && selected_peer_ == peer;
}

network::transport::DatagramSendResult WebRtcTransport::Send(
    network::transport::DatagramProtocol protocol,
    const uint8_t* data,
    size_t size)
{
    network::SocketAddr peer;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!has_selected_peer_)
        {
            return network::transport::DatagramSendResult::NotWritable;
        }
        peer = selected_peer_;
    }
    return SendTo(peer, protocol, data, size);
}

network::transport::DatagramSendResult WebRtcTransport::SendTo(
    const network::SocketAddr& peer,
    network::transport::DatagramProtocol protocol,
    const uint8_t* data,
    size_t size)
{
    if (peer.len == 0 ||
        protocol == network::transport::DatagramProtocol::Unknown ||
        !data || size == 0)
    {
        return network::transport::DatagramSendResult::Failed;
    }
    if (!datagram_transport_ || !datagram_transport_->IsWritable())
    {
        return network::transport::DatagramSendResult::Closed;
    }
    return datagram_transport_->SendDatagram(peer, data, size);
}

void WebRtcTransport::OnDatagram(
    network::transport::ReceivedDatagram datagram)
{
    const auto state = State();
    if (!datagram.IsValid() ||
        (state != WebRtcTransportState::Connecting &&
         state != WebRtcTransportState::Connected))
    {
        return;
    }

    const auto protocol = network::transport::DatagramProtocolClassifier::Classify(
        datagram.Data(), datagram.Size());
    if (protocol == network::transport::DatagramProtocol::Unknown)
    {
        return;
    }
    if (protocol != network::transport::DatagramProtocol::Stun &&
        !IsSelectedPeer(datagram.remote))
    {
        return;
    }

    std::shared_ptr<IWebRtcTransportSink> sink;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        sink = sink_.lock();
    }
    if (sink)
    {
        sink->OnWebRtcDatagram(protocol, std::move(datagram));
    }
}

} // namespace protocol::webrtc
