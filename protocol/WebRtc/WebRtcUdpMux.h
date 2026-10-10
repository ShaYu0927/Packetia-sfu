#ifndef _WEBRTC_UDP_MUX_H_
#define _WEBRTC_UDP_MUX_H_

#include "transport/UdpDatagramTransport.h"

#include <string>
#include <unordered_map>

namespace protocol::webrtc
{

// Shared ICE-lite UDP listener. The owner serializes registration, packet
// callbacks, peer selection and cleanup on the UdpServer's TaskScheduler.
class WebRtcUdpMux final : public network::IUdpHandler
{
public:
    explicit WebRtcUdpMux(std::weak_ptr<network::UdpServer> server);
    ~WebRtcUdpMux() override;

    std::shared_ptr<network::transport::UdpDatagramTransport> Register(
        uint64_t id, const std::string& localUfrag);
    void Unregister(const std::string& localUfrag);
    // Atomically replace the ICE generation without closing its adapter.
    bool Restart(const std::string& oldUfrag, const std::string& newUfrag);

    // Call only after the session authenticates an ICE nomination. Routing a
    // STUN request by USERNAME alone never establishes a media peer mapping.
    bool BindPeer(const std::string& localUfrag, const network::SocketAddr& peer);
    void Close();

    void OnDatagram(const network::SocketAddr& source,
                    const uint8_t* data, size_t size) override;
    void OnClosed(int reason) override;
    void OnError(int error) override;

private:
    struct Entry
    {
        std::shared_ptr<network::transport::UdpDatagramTransport> transport;
        network::SocketAddr peer;
    };

    std::weak_ptr<network::UdpServer> server_;
    std::unordered_map<std::string, Entry> sessions_;
    std::unordered_map<network::SocketAddr, std::string, network::SocketAddrHash> peers_;
    bool closed_ = false;
};

} // namespace protocol::webrtc

#endif /* _WEBRTC_UDP_MUX_H_ */
