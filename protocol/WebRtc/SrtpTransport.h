#ifndef _SRTP_TRANSPORT_H_
#define _SRTP_TRANSPORT_H_

#include <cstdint>
#include <vector>

namespace protocol::webrtc
{

struct SrtpKeyingMaterial
{
    // IANA DTLS-SRTP protection profile ID. Keys include the master salt.
    uint16_t profile = 0;
    std::vector<uint8_t> sendKey;
    std::vector<uint8_t> receiveKey;

    ~SrtpKeyingMaterial()
    {
        for (auto& byte : sendKey) *static_cast<volatile uint8_t*>(&byte) = 0;
        for (auto& byte : receiveKey) *static_cast<volatile uint8_t*>(&byte) = 0;
    }
};

// Implement with an SRTP library. Unprotect must authenticate and enforce replay
// protection before returning plaintext; failure must never deliver a packet.
class SrtpTransport
{
public:
    SrtpTransport() = default;
    virtual ~SrtpTransport() = default;

    SrtpTransport(const SrtpTransport&) = delete;
    SrtpTransport& operator=(const SrtpTransport&) = delete;

    virtual bool Configure(const SrtpKeyingMaterial& keys) = 0;
    virtual bool UnprotectRtp(std::vector<uint8_t>& packet) = 0;
    virtual bool UnprotectRtcp(std::vector<uint8_t>& packet) = 0;
    virtual bool ProtectRtp(std::vector<uint8_t>& packet) = 0;
    virtual bool ProtectRtcp(std::vector<uint8_t>& packet) = 0;
    // Idempotent; destroys both SRTP contexts and their key material.
    virtual void Close() noexcept = 0;
};

} // namespace protocol::webrtc

#endif /* _SRTP_TRANSPORT_H_ */
