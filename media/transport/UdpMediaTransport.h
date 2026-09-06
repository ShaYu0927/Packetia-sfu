#ifndef _UDP_MEDIA_TRANSPORT_H_
#define _UDP_MEDIA_TRANSPORT_H_

#include "MediaTransportBase.h"
#include "transport/IDatagramTransport.h"
#include "UdpServer.h"

#include <array>
#include <memory>
#include <mutex>

namespace media::transport
{
// Separate ports bind packet roles at setup. The single-transport constructor
// explicitly represents RTP/RTCP mux; only that mode inspects the packet type.
class UdpMediaTransport final : public MediaTransportBase,
                                public std::enable_shared_from_this<UdpMediaTransport>
{
public:
    using DatagramTransport = network::transport::IDatagramTransport;
    UdpMediaTransport(uint64_t id, std::shared_ptr<DatagramTransport> mux);
    UdpMediaTransport(uint64_t id, std::shared_ptr<DatagramTransport> rtp,
                      std::shared_ptr<DatagramTransport> rtcp);
    ~UdpMediaTransport() override;

    // Reserve an exclusive even/odd socket pair on the RTSP connection's scheduler.
    static std::shared_ptr<UdpMediaTransport> CreateUnicast(
        uint64_t id, std::shared_ptr<TaskScheduler> scheduler,
        const std::string& local_ip, const std::string& peer_ip,
        uint16_t client_rtp_port, uint16_t client_rtcp_port);

    bool Start();
    bool IsRtcpMux() const noexcept { return rtcp_mux_; }
    uint16_t LocalPort(MediaPacketType type) const;
    MediaTransportProtocol Protocol() const noexcept override;
    SendResult Send(MediaPacketType type, const uint8_t* data, size_t size,
                    bool retransmit = false) override;
    void Close() override;

    void SetSelectedPeer(const network::SocketAddr& peer);
    void SetSelectedPeers(const network::SocketAddr& rtp, const network::SocketAddr& rtcp);
    bool IsSelectedPeer(const network::SocketAddr& peer) const;
    bool IsSelectedPeer(MediaPacketType type, const network::SocketAddr& peer) const;
    MediaPacketIngressResult InputDatagram(
        const network::SocketAddr& source, MediaPacketType type,
        const uint8_t* data, size_t size, uint64_t receive_time_ms);

private:
    static size_t Index(MediaPacketType type) { return type == MediaPacketType::Rtcp ? 1 : 0; }
    const bool rtcp_mux_;
    mutable std::mutex mutex_;
    std::array<std::shared_ptr<DatagramTransport>, 2> datagrams_;
    std::array<std::shared_ptr<network::transport::IDatagramSink>, 2> port_sinks_;
    std::array<std::shared_ptr<network::UdpServer>, 2> servers_;
    std::array<network::SocketAddr, 2> peers_{};
    std::array<bool, 2> peer_confirmed_{{true, true}};
    bool started_ = false;
};
}
#endif
