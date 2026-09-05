#ifndef PACKETIA_PROTOCOL_WEBRTC_TRANSPORT_H_
#define PACKETIA_PROTOCOL_WEBRTC_TRANSPORT_H_

#include "transport/DatagramProtocolClassifier.h"
#include "transport/IDatagramTransport.h"

#include <atomic>
#include <memory>
#include <mutex>

namespace protocol::webrtc
{

enum class WebRtcTransportState
{
    Created = 0,
    Connecting,
    Connected,
    Closing,
    Closed,
    Failed
};

class IWebRtcTransportSink
{
public:
    virtual ~IWebRtcTransportSink() = default;

    virtual void OnWebRtcDatagram(
        network::transport::DatagramProtocol protocol,
        network::transport::ReceivedDatagram datagram) = 0;

    virtual void OnWebRtcTransportClosed() {}
};

/*
 * WebRTC packet transport boundary.
 *
 * This class owns demultiplexing, selected-peer validation and raw datagram
 * I/O. ICE, DTLS and SRTP processing are deliberately supplied by its sink.
 */
class WebRtcTransport final
    : public network::transport::IDatagramSink,
      public std::enable_shared_from_this<WebRtcTransport>
{
public:
    WebRtcTransport(
        uint64_t id,
        std::shared_ptr<network::transport::IDatagramTransport> datagram_transport);

    uint64_t Id() const noexcept { return id_; }
    WebRtcTransportState State() const noexcept;

    bool Start();
    void Close();

    void SetSink(std::weak_ptr<IWebRtcTransportSink> sink);
    bool SelectPeer(const network::SocketAddr& peer);
    bool IsSelectedPeer(const network::SocketAddr& peer) const;

    network::transport::DatagramSendResult Send(
        network::transport::DatagramProtocol protocol,
        const uint8_t* data,
        size_t size);

    network::transport::DatagramSendResult SendTo(
        const network::SocketAddr& peer,
        network::transport::DatagramProtocol protocol,
        const uint8_t* data,
        size_t size);

    void OnDatagram(network::transport::ReceivedDatagram datagram) override;

private:
    const uint64_t id_;
    std::shared_ptr<network::transport::IDatagramTransport> datagram_transport_;
    std::atomic<WebRtcTransportState> state_{WebRtcTransportState::Created};

    mutable std::mutex mutex_;
    network::SocketAddr selected_peer_{};
    bool has_selected_peer_{false};
    std::weak_ptr<IWebRtcTransportSink> sink_;
};

} // namespace protocol::webrtc

#endif /* PACKETIA_PROTOCOL_WEBRTC_TRANSPORT_H_ */
