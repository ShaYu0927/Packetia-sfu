#ifndef _DTLS_TRANSPORT_H_
#define _DTLS_TRANSPORT_H_

#include "WebRtc_Config.h"
#include "SrtpTransport.h"

#include <cstddef>
#include <functional>

namespace protocol::webrtc
{

// Backend calls are serialized by the session owner. The send callback must run
// on that same thread. IsConnected means the peer certificate fingerprint was
// verified against Configure(), not merely that the TLS handshake completed.
class DtlsTransport
{
public:
    DtlsTransport() = default;
    virtual ~DtlsTransport() = default;

    DtlsTransport(const DtlsTransport&) = delete;
    DtlsTransport& operator=(const DtlsTransport&) = delete;

    using SendCallback = std::function<bool(const uint8_t*, size_t)>;

    virtual DtlsParameters LocalParameters() const = 0;
    virtual bool Configure(const DtlsParameters& remote, DtlsSetup localSetup) = 0;
    virtual bool Start(SendCallback send) = 0;
    // False denotes a fatal backend/handshake error, not an ignorable packet.
    virtual bool HandleDatagram(const uint8_t* data, size_t size) = 0;
    // Drive retransmissions and the handshake deadline using monotonic time.
    virtual bool Tick(uint64_t nowMs) = 0;
    virtual bool IsConnected() const noexcept = 0;
    // Export EXTRACTOR-dtls_srtp and map client/server keys to send/receive keys.
    virtual bool ExportSrtpKeys(SrtpKeyingMaterial& keys) const = 0;
    // Idempotent; releases keys/certificate state and detaches send callbacks.
    virtual void Close() noexcept = 0;
};

} // namespace protocol::webrtc

#endif /* _DTLS_TRANSPORT_H_ */
