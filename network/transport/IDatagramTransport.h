#ifndef _NETWORK_TRANSPORT_I_DATAGRAM_TRANSPORT_H_
#define _NETWORK_TRANSPORT_I_DATAGRAM_TRANSPORT_H_

#include "DatagramPacket.h"

#include <memory>
#include <functional>

namespace network::transport
{

enum class DatagramSendResult
{
    Ok = 0,
    Failed,
    Closed,
    NotWritable
};

class IDatagramSink
{
public:
    virtual ~IDatagramSink() = default;
    virtual void OnDatagram(ReceivedDatagram datagram) = 0;
};

/* Raw, protocol-neutral datagram I/O boundary. */
class IDatagramTransport
{
public:
    virtual ~IDatagramTransport() = default;

    virtual uint64_t Id() const noexcept = 0;
    virtual bool IsWritable() const noexcept = 0;

    virtual DatagramSendResult SendDatagram(
        const network::SocketAddr& remote,
        const uint8_t* data,
        size_t size) = 0;

    // Completion runs only after a successful send; queued implementations
    // override this to propagate it to their actual I/O boundary.
    virtual DatagramSendResult SendDatagramWithCompletion(
        const network::SocketAddr& remote, const uint8_t* data, size_t size,
        std::function<bool()> valid, std::function<void()> on_sent)
    {
        if (valid && !valid()) return DatagramSendResult::Closed;
        const auto result = SendDatagram(remote, data, size);
        if (result == DatagramSendResult::Ok && on_sent) on_sent();
        return result;
    }

    virtual void SetDatagramSink(std::weak_ptr<IDatagramSink> sink) = 0;
    virtual void Close() = 0;
};

} // namespace network::transport

#endif /* _NETWORK_TRANSPORT_I_DATAGRAM_TRANSPORT_H_ */
