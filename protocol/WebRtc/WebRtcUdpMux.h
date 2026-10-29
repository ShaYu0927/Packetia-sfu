#ifndef _WEBRTC_UDP_MUX_H_
#define _WEBRTC_UDP_MUX_H_

#include "UdpDatagramTransport.h"

#include <string>
#include <mutex>
#include <atomic>
#include <unordered_map>

namespace protocol::webrtc
{

// 共享 ICE-lite UDP 入口。路由表由短锁保护，消息在锁外投递到会话所属执行器。
class WebRtcUdpMux final : public network::IUdpHandler
{
public:
    using Dispatch = std::function<bool(std::function<void()>, size_t)>;
    explicit WebRtcUdpMux(std::weak_ptr<network::UdpServer> server,
                         network::transport::UdpDatagramTransport::Sender sender = {});
    ~WebRtcUdpMux() override;

    std::shared_ptr<network::transport::UdpDatagramTransport> Register(
        uint64_t id, const std::string& localUfrag, Dispatch dispatch = {});
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
        Dispatch dispatch;
        std::shared_ptr<std::atomic<bool>> active = std::make_shared<std::atomic<bool>>(true);
    };

    std::weak_ptr<network::UdpServer> server_;
    const network::transport::UdpDatagramTransport::Sender sender_;
    std::mutex mutex_;
    std::unordered_map<std::string, Entry> sessions_;
    std::unordered_map<network::SocketAddr, std::string, network::SocketAddrHash> peers_;
    bool closed_ = false;
};

} // namespace protocol::webrtc

#endif /* _WEBRTC_UDP_MUX_H_ */
