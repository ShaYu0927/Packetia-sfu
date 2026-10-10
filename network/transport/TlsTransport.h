#ifndef PACKETIA_NETWORK_TLS_TRANSPORT_H
#define PACKETIA_NETWORK_TLS_TRANSPORT_H

#include "TlsContext.h"
#include "../TcpConnection.h"

#include <atomic>
#include <functional>
#include <memory>

namespace network::transport
{

// Server-side TLS over an accepted TCP connection. Owns its byte, close and
// write-complete callbacks; the server's disconnect callback remains intact.
class TlsTransport : public std::enable_shared_from_this<TlsTransport>
{
public:
    enum class State { Created, Handshaking, Open, Closing, Closed, Failed };
    enum class Error { None, LocalClose, TcpClosed, HandshakeTimeout, ProtocolError,
                       BufferLimit, IoError };
    enum class SendResult { Queued, NotWritable, Closed, Failed };

    struct Options
    {
        uint32_t handshake_timeout_ms = 10000;
        uint32_t shutdown_timeout_ms = 2000;
        size_t max_plaintext_bytes = 1024 * 1024;
        size_t max_input_bytes = 256 * 1024;
        size_t max_ciphertext_bytes = 256 * 1024;
    };

    struct Callbacks
    {
        std::function<void()> on_ready;
        // Borrowed plaintext, valid only during the owner-thread callback.
        std::function<void(const uint8_t*, size_t)> on_bytes;
        std::function<void(Error)> on_closed;
    };

    TlsTransport(TcpConnection::Ptr connection, std::shared_ptr<TlsContext> context);
    TlsTransport(TcpConnection::Ptr connection, std::shared_ptr<TlsContext> context,
                 Options options);
    ~TlsTransport();

    bool Start(Callbacks callbacks);
    SendResult Send(const uint8_t* data, size_t size);
    void Close();
    State GetState() const noexcept;
    Error LastError() const noexcept;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace network::transport

#endif
