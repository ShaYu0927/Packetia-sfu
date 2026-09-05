#ifndef _NETWORK_TRANSPORT_UDP_DATAGRAM_TRANSPORT_H_
#define _NETWORK_TRANSPORT_UDP_DATAGRAM_TRANSPORT_H_

#include "IDatagramTransport.h"
#include "UdpServer.h"

#include <atomic>
#include <mutex>

namespace network::transport
{

/*
 * Adapts UdpServer to the protocol-neutral datagram boundary.
 * The owner installs this object as UdpServer's handler.
 */
class UdpDatagramTransport final : public IDatagramTransport,
                                   public network::IUdpHandler
{
public:
    UdpDatagramTransport(uint64_t id,
                         std::weak_ptr<network::UdpServer> server) noexcept;

    uint64_t Id() const noexcept override;
    bool IsWritable() const noexcept override;
    DatagramSendResult SendDatagram(const network::SocketAddr& remote,
                                    const uint8_t* data,
                                    size_t size) override;
    void SetDatagramSink(std::weak_ptr<IDatagramSink> sink) override;
    void Close() override;

    void OnDatagram(const network::SocketAddr& source,
                    const uint8_t* data,
                    size_t size) override;
    void OnClosed(int reason) override;
    void OnError(int error) override;

private:
    const uint64_t id_;
    std::weak_ptr<network::UdpServer> server_;
    std::atomic<bool> closed_{false};
    std::mutex sink_mutex_;
    std::weak_ptr<IDatagramSink> sink_;
};

} // namespace network::transport

#endif /* _NETWORK_TRANSPORT_UDP_DATAGRAM_TRANSPORT_H_ */
