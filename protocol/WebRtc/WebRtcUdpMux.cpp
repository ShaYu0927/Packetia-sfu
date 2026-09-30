#include "WebRtcUdpMux.h"

#include "Stun.h"
#include "transport/DatagramProtocolClassifier.h"

#include <utility>

namespace protocol::webrtc
{

WebRtcUdpMux::WebRtcUdpMux(std::weak_ptr<network::UdpServer> server)
    : server_(std::move(server))
{
}

WebRtcUdpMux::~WebRtcUdpMux() { Close(); }

std::shared_ptr<network::transport::UdpDatagramTransport> WebRtcUdpMux::Register(
    uint64_t id, const std::string& localUfrag)
{
    if (closed_ || id == 0 || server_.expired() || sessions_.count(localUfrag) ||
        localUfrag.size() < 4 || localUfrag.size() > 256 ||
        localUfrag.find_first_not_of("abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789+/") != std::string::npos)
        return {};

    auto transport = std::make_shared<network::transport::UdpDatagramTransport>(id, server_);
    sessions_.emplace(localUfrag, Entry{transport, {}});
    return transport;
}

void WebRtcUdpMux::Unregister(const std::string& localUfrag)
{
    const auto session = sessions_.find(localUfrag);
    if (session == sessions_.end()) return;
    if (session->second.peer.len != 0) peers_.erase(session->second.peer);
    session->second.transport->Close();
    sessions_.erase(session);
}

bool WebRtcUdpMux::BindPeer(const std::string& localUfrag, const network::SocketAddr& peer)
{
    const auto session = sessions_.find(localUfrag);
    if (closed_ || session == sessions_.end() || !session->second.transport->IsWritable() ||
        (!peer.IsV4() && !peer.IsV6()) || peer.Port() == 0 ||
        peer.len != (peer.IsV4() ? sizeof(sockaddr_in) : sizeof(sockaddr_in6)))
        return false;

    Entry* previousOwner = nullptr;
    const auto existing = peers_.find(peer);
    if (existing != peers_.end() && existing->second != localUfrag)
    {
        const auto owner = sessions_.find(existing->second);
        if (owner != sessions_.end() && owner->second.transport->IsWritable()) return false;
        // An explicitly closed adapter no longer owns its selected address.
        if (owner != sessions_.end()) previousOwner = &owner->second;
    }
    // Insert first: allocation failure must leave the previous binding intact.
    peers_.insert_or_assign(peer, localUfrag);
    if (previousOwner) previousOwner->peer = {};
    if (session->second.peer.len != 0 && !(session->second.peer == peer))
        peers_.erase(session->second.peer);
    session->second.peer = peer;
    return true;
}

void WebRtcUdpMux::Close()
{
    if (closed_) return;
    closed_ = true;
    for (const auto& session : sessions_) session.second.transport->Close();
    peers_.clear();
    sessions_.clear();
}

void WebRtcUdpMux::OnDatagram(const network::SocketAddr& source,
                             const uint8_t* data, size_t size)
try
{
    if (closed_ || !data || size == 0 || source.len == 0) return;
    using Classifier = network::transport::DatagramProtocolClassifier;
    using Protocol = network::transport::DatagramProtocol;
    const auto protocol = Classifier::Classify(data, size);
    auto session = sessions_.end();
    if (protocol == Protocol::Stun)
    {
        StunMessageInfo message;
        std::string_view username;
        if (!StunCodec::Parse(data, size, message) || message.raw_len != size ||
            !message.IsBindingRequest() || !StunCodec::DecodeUsername(message, username)) return;
        const auto separator = username.find(':');
        if (separator == std::string_view::npos || separator < 4 || separator > 256 ||
            separator + 1 == username.size() || username.find(':', separator + 1) != std::string_view::npos) return;
        session = sessions_.find(std::string(username.substr(0, separator)));
    }
    else if (protocol != Protocol::Unknown)
    {
        const auto peer = peers_.find(source);
        if (peer != peers_.end()) session = sessions_.find(peer->second);
    }
    if (session == sessions_.end()) return;
    // Delivery may synchronously unregister the session; retain its adapter.
    auto transport = session->second.transport;
    transport->OnDatagram(source, data, size);
}
catch (const std::bad_alloc&) {} // Drop under memory pressure; keep IO alive.

void WebRtcUdpMux::OnClosed(int /*reason*/) { Close(); }
void WebRtcUdpMux::OnError(int /*error*/) { Close(); }

} // namespace protocol::webrtc
