#ifndef _NETWORK_TRANSPORT_I_DATAGRAM_TRANSPORT_H_
#define _NETWORK_TRANSPORT_I_DATAGRAM_TRANSPORT_H_

#include "DatagramPacket.h"

#include <memory>

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

    virtual void SetDatagramSink(std::weak_ptr<IDatagramSink> sink) = 0;
    virtual void Close() = 0;
};

} // namespace network::transport

#endif /* _NETWORK_TRANSPORT_I_DATAGRAM_TRANSPORT_H_ */
