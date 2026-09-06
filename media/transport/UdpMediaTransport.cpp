#include "UdpMediaTransport.h"
#include "network/transport/UdpDatagramTransport.h"
#include "network/transport/DatagramProtocolClassifier.h"

#include <functional>
#include <utility>

namespace media::transport
{
namespace {
class PortSink final : public network::transport::IDatagramSink {
public:
    explicit PortSink(std::function<void(network::transport::ReceivedDatagram)> callback)
        : callback_(std::move(callback)) {}
    void OnDatagram(network::transport::ReceivedDatagram packet) override {
        callback_(std::move(packet));
    }
private:
    std::function<void(network::transport::ReceivedDatagram)> callback_;
};

bool SameHost(const network::SocketAddr& a, const network::SocketAddr& b) {
    return (a.IsV4() && b.IsV4() && a.IPv4Bytes() == b.IPv4Bytes()) ||
           (a.IsV6() && b.IsV6() && a.IPv6Bytes() == b.IPv6Bytes());
}

// Validate the role already bound to this port. Do not treat marker + RTP
// PT 72 (second byte 200) as RTCP on a dedicated RTP socket.
bool ValidPacket(MediaPacketType type, const uint8_t* data, size_t size) {
    if (!data || size < 4 || (data[0] >> 6) != 2) return false;
    if (type == MediaPacketType::Rtcp) {
        const size_t length = (((size_t(data[2]) << 8) | data[3]) + 1) * 4;
        return data[1] >= 192 && data[1] <= 223 && length >= 4 && length <= size;
    }
    size_t header = 12 + (data[0] & 0x0F) * 4;
    if (header > size) return false;
    if (data[0] & 0x10) {
        if (header + 4 > size) return false;
        header += 4 + ((size_t(data[header + 2]) << 8) | data[header + 3]) * 4;
        if (header > size) return false;
    }
    return !(data[0] & 0x20) || (data[size - 1] > 0 && data[size - 1] <= size - header);
}
}

UdpMediaTransport::UdpMediaTransport(uint64_t id, std::shared_ptr<DatagramTransport> mux)
    : MediaTransportBase(id), rtcp_mux_(true), datagrams_{{mux, mux}}
{
    SetState(id && mux && mux->IsWritable() ? MediaTransportState::Connecting : MediaTransportState::Failed);
}

UdpMediaTransport::UdpMediaTransport(uint64_t id, std::shared_ptr<DatagramTransport> rtp,
                                   std::shared_ptr<DatagramTransport> rtcp)
    : MediaTransportBase(id), rtcp_mux_(false), datagrams_{{rtp, rtcp}}
{
    SetState(id && rtp && rtcp && rtp != rtcp && rtp->IsWritable() && rtcp->IsWritable()
        ? MediaTransportState::Connecting : MediaTransportState::Failed);
}

UdpMediaTransport::~UdpMediaTransport() { Close(); }

std::shared_ptr<UdpMediaTransport> UdpMediaTransport::CreateUnicast(
    uint64_t id, std::shared_ptr<TaskScheduler> scheduler,
    const std::string& local_ip, const std::string& peer_ip,
    uint16_t client_rtp_port, uint16_t client_rtcp_port)
{
    if (!id || !scheduler || scheduler->IsStopped() || !client_rtp_port ||
        !client_rtcp_port || client_rtp_port == client_rtcp_port) return {};

    auto rtp_server = std::make_shared<network::UdpServer>(scheduler);
    auto rtcp_server = std::make_shared<network::UdpServer>(scheduler);
    bool allocated = false;
    for (int attempt = 0; attempt < 64; ++attempt) {
        if (!rtp_server->Start(local_ip, 0, false)) return {};
        const auto port = rtp_server->LocalAddress().Port();
        if (port && (port % 2 == 0) && port < 65535 &&
            rtcp_server->Start(local_ip, port + 1, false)) {
            allocated = true;
            break;
        }
        rtp_server->Stop();
    }
    if (!allocated) return {};
    auto rtp = std::make_shared<network::transport::UdpDatagramTransport>(id, rtp_server);
    auto rtcp = std::make_shared<network::transport::UdpDatagramTransport>(id, rtcp_server);
    auto result = std::make_shared<UdpMediaTransport>(id, rtp, rtcp);
    result->servers_ = {{rtp_server, rtcp_server}};
    result->SetSelectedPeers(network::SocketAddr::FromIPPort(peer_ip, client_rtp_port),
                             network::SocketAddr::FromIPPort(peer_ip, client_rtcp_port));
    // Learn the first valid NAT source port from the negotiated host per socket,
    // then pin it. Other source IPs cannot alter the binding.
    result->peer_confirmed_ = {{false, false}};
    rtp_server->SetHandler(rtp);
    rtcp_server->SetHandler(rtcp);
    if (!result->Start()) return {};
    return result;
}

bool UdpMediaTransport::Start()
{
    std::lock_guard<std::mutex> lock(mutex_);
    if (IsClosed()) return false;
    if (started_) return true;
    auto weak = weak_from_this();
    if (weak.expired()) return false;
    const size_t count = rtcp_mux_ ? 1 : 2;
    for (size_t i = 0; i < count; ++i) {
        port_sinks_[i] = std::make_shared<PortSink>([weak, i](network::transport::ReceivedDatagram packet) {
            auto self = weak.lock();
            if (!self || !packet.IsValid()) return;
            MediaPacketType type = i == 0 ? MediaPacketType::Rtp : MediaPacketType::Rtcp;
            if (self->rtcp_mux_) {
                using namespace network::transport;
                const auto protocol = DatagramProtocolClassifier::Classify(packet.Data(), packet.Size());
                if (protocol == DatagramProtocol::Rtcp) type = MediaPacketType::Rtcp;
                else if (protocol != DatagramProtocol::Rtp) return;
            }
            self->InputDatagram(packet.remote, type, packet.Data(), packet.Size(), packet.receive_time_ms);
        });
        datagrams_[i]->SetDatagramSink(port_sinks_[i]);
    }
    started_ = true;
    return true;
}

uint16_t UdpMediaTransport::LocalPort(MediaPacketType type) const
{
    std::shared_ptr<network::UdpServer> server;
    { std::lock_guard<std::mutex> lock(mutex_); server = servers_[Index(type)]; }
    return server ? server->LocalAddress().Port() : 0;
}

MediaTransportProtocol UdpMediaTransport::Protocol() const noexcept { return MediaTransportProtocol::Udp; }

SendResult UdpMediaTransport::Send(MediaPacketType type, const uint8_t* data,
                                 size_t size, bool retransmit)
{
    (void)retransmit;
    if (!data || !size) return SendResult::Failed;
    std::shared_ptr<DatagramTransport> datagram;
    network::SocketAddr peer;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (State() != MediaTransportState::Connected)
            return IsClosed() ? SendResult::Closed : SendResult::NotWritable;
        datagram = datagrams_[Index(type)];
        peer = peers_[Index(type)];
    }
    if (!datagram || !datagram->IsWritable()) return SendResult::Closed;
    switch (datagram->SendDatagram(peer, data, size)) {
        case network::transport::DatagramSendResult::Ok: return SendResult::Ok;
        case network::transport::DatagramSendResult::Closed: return SendResult::Closed;
        case network::transport::DatagramSendResult::NotWritable: return SendResult::NotWritable;
        default: return SendResult::Failed;
    }
}

void UdpMediaTransport::Close()
{
    std::array<std::shared_ptr<DatagramTransport>, 2> datagrams;
    std::array<std::shared_ptr<network::UdpServer>, 2> servers;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (State() == MediaTransportState::Closed) return;
        SetState(MediaTransportState::Closed);
        datagrams.swap(datagrams_);
        servers.swap(servers_);
        port_sinks_ = {};
        peers_ = {};
    }
    DetachPacketSink();
    for (auto& datagram : datagrams) if (datagram) datagram->SetDatagramSink({});
    // Stop may wait for a callback, so never hold the transport mutex here.
    for (auto& server : servers) if (server) server->Stop();
}

void UdpMediaTransport::SetSelectedPeer(const network::SocketAddr& peer) { SetSelectedPeers(peer, peer); }

void UdpMediaTransport::SetSelectedPeers(const network::SocketAddr& rtp, const network::SocketAddr& rtcp)
{
    std::lock_guard<std::mutex> lock(mutex_);
    if (IsClosed() || !rtp.len || !rtcp.len || (rtcp_mux_ && !(rtp == rtcp))) return;
    peers_ = {{rtp, rtcp}};
    peer_confirmed_ = {{true, true}};
    SetState(MediaTransportState::Connected);
}

bool UdpMediaTransport::IsSelectedPeer(const network::SocketAddr& peer) const
{
    return IsSelectedPeer(MediaPacketType::Rtp, peer) || IsSelectedPeer(MediaPacketType::Rtcp, peer);
}

bool UdpMediaTransport::IsSelectedPeer(MediaPacketType type, const network::SocketAddr& peer) const
{
    std::lock_guard<std::mutex> lock(mutex_);
    return State() == MediaTransportState::Connected && peers_[Index(type)] == peer;
}

MediaPacketIngressResult UdpMediaTransport::InputDatagram(
    const network::SocketAddr& source, MediaPacketType type,
    const uint8_t* data, size_t size, uint64_t receive_time_ms)
{
    if (!ValidPacket(type, data, size)) return MediaPacketIngressResult::Dropped;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (State() != MediaTransportState::Connected) return MediaPacketIngressResult::Closed;
        const auto index = Index(type);
        if (!peer_confirmed_[index] && SameHost(source, peers_[index])) {
            peers_[index] = source;
            peer_confirmed_[index] = true;
        }
        if (!(peers_[index] == source)) return MediaPacketIngressResult::Dropped;
    }
    return PublishPacket(type, data, size, receive_time_ms);
}
}
