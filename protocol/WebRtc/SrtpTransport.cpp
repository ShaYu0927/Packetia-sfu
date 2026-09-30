#include "SrtpTransport.h"

#if defined(PACKETIA_WITH_SRTP) && PACKETIA_WITH_SRTP
#include <srtp2/srtp.h>

#include <limits>
#include <mutex>
#endif

namespace protocol::webrtc
{
#if defined(PACKETIA_WITH_SRTP) && PACKETIA_WITH_SRTP
namespace
{
constexpr uint16_t kAes128CmSha1_80 = 1;
constexpr size_t kMasterKeySize = 30;
constexpr size_t kMaxDatagram = 65507;

void ClearPacket(std::vector<uint8_t>& packet)
{
    for (auto& byte : packet) *static_cast<volatile uint8_t*>(&byte) = 0;
    packet.clear();
}

bool InitializeSrtp()
{
    static std::once_flag once;
    static bool initialized = false;
    std::call_once(once, [] { initialized = srtp_init() == srtp_err_status_ok; });
    // The library stays initialized for the process lifetime; one transport
    // closing must not tear down another transport's crypto implementation.
    return initialized;
}

class LibSrtpTransport final : public SrtpTransport
{
public:
    ~LibSrtpTransport() override { Close(); }

    bool Configure(const SrtpKeyingMaterial& keys) override
    {
        if (closed_ || send_ || receive_ || keys.profile != kAes128CmSha1_80 ||
            keys.sendKey.size() != kMasterKeySize || keys.receiveKey.size() != kMasterKeySize) return false;
        if (!CreateContext(send_, keys.sendKey, ssrc_any_outbound) ||
            !CreateContext(receive_, keys.receiveKey, ssrc_any_inbound))
        {
            Close();
            return false;
        }
        return true;
    }

    bool UnprotectRtp(std::vector<uint8_t>& packet) override { return Transform(packet, false, false); }
    bool UnprotectRtcp(std::vector<uint8_t>& packet) override { return Transform(packet, false, true); }
    bool ProtectRtp(std::vector<uint8_t>& packet) override { return Transform(packet, true, false); }
    bool ProtectRtcp(std::vector<uint8_t>& packet) override { return Transform(packet, true, true); }

    void Close() noexcept override
    {
        closed_ = true;
        if (send_) srtp_dealloc(send_);
        if (receive_) srtp_dealloc(receive_);
        send_ = nullptr;
        receive_ = nullptr;
    }

private:
    static bool CreateContext(srtp_t& context, const std::vector<uint8_t>& key, srtp_ssrc_type_t type)
    {
        srtp_policy_t policy{};
        srtp_crypto_policy_set_aes_cm_128_hmac_sha1_80(&policy.rtp);
        srtp_crypto_policy_set_aes_cm_128_hmac_sha1_80(&policy.rtcp);
        policy.ssrc.type = type;
        // srtp_create derives and retains its own keys synchronously.
        policy.key = const_cast<unsigned char*>(key.data());
        policy.window_size = 1024;
        policy.allow_repeat_tx = 0;
        return srtp_create(&context, &policy) == srtp_err_status_ok;
    }

    bool Transform(std::vector<uint8_t>& packet, bool protect, bool rtcp)
    {
        const auto context = protect ? send_ : receive_;
        // SRTCP also appends its four-byte index. Reserve the library maximum
        // even though the negotiated profile currently has a ten-byte tag.
        constexpr size_t tail = SRTP_MAX_TRAILER_LEN + 4;
        if (closed_ || !context || packet.size() < (rtcp ? 8u : 12u) || packet.size() > kMaxDatagram ||
            packet.size() > static_cast<size_t>(std::numeric_limits<int>::max()) - tail ||
            (protect && packet.size() > kMaxDatagram - tail))
        {
            ClearPacket(packet);
            return false;
        }
        int size = static_cast<int>(packet.size());
        try
        {
            if (protect) packet.resize(packet.size() + tail);
        }
        catch (...)
        {
            ClearPacket(packet);
            return false;
        }
        srtp_err_status_t result;
        if (protect)
        {
            result = rtcp ? srtp_protect_rtcp(context, packet.data(), &size) :
                            srtp_protect(context, packet.data(), &size);
        }
        else
        {
            result = rtcp ? srtp_unprotect_rtcp(context, packet.data(), &size) :
                            srtp_unprotect(context, packet.data(), &size);
        }
        if (result != srtp_err_status_ok || size <= 0 || static_cast<size_t>(size) > packet.size())
        {
            // Never leave partially decrypted bytes visible on authentication,
            // replay, malformed-packet, or backend failure.
            ClearPacket(packet);
            return false;
        }
        packet.resize(static_cast<size_t>(size));
        return true;
    }

    srtp_t send_ = nullptr;
    srtp_t receive_ = nullptr;
    bool closed_ = false;
};
} // namespace
#endif

std::unique_ptr<SrtpTransport> CreateSrtpTransport()
{
#if defined(PACKETIA_WITH_SRTP) && PACKETIA_WITH_SRTP
    try
    {
        if (InitializeSrtp()) return std::make_unique<LibSrtpTransport>();
    }
    catch (...)
    {
    }
#endif
    return nullptr;
}

} // namespace protocol::webrtc
