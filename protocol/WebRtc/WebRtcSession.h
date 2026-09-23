#ifndef _WEBRTC_SESSION_H_
#define _WEBRTC_SESSION_H_

#include "WebRtcTransport.h"
#include "WebRtc_Config.h"
#include "IceAgent.h"
#include "DtlsTransport.h"
#include "SrtpTransport.h"
#include "IMediaPacketSink.h"

#include <memory>
#include <atomic>

namespace protocol::webrtc
{

enum class WebRtcSessionState
{
    New, HaveOffer, HaveAnswer, Connecting, Connected, Closed, Failed
};

struct WebRtcSessionOptions
{
    // Supply fresh cryptographically random credentials and gathered candidates.
    IceParameters ice;
    // One capability entry per media kind. H264 and Opus use codec-specific
    // fmtp negotiation. Repair codecs (RTX/RED/FEC) are not advertised.
    std::vector<WebRtcMediaDescription> medias;
    sdp::SdpOrigin origin;
};

// ICE-lite answerer, one transport, RTP/RTCP mux, optional single BUNDLE group.
// The owner must serialize mutations (including transport callbacks and Tick)
// on one event loop. State() may be read by media workers. Renegotiation/ICE
// restart require a new session for now.
class WebRtcSession : public IWebRtcTransportSink, public std::enable_shared_from_this<WebRtcSession>
{
public:
    WebRtcSession(std::shared_ptr<WebRtcTransport> transport,
                  std::unique_ptr<DtlsTransport> dtls,
                  std::unique_ptr<SrtpTransport> srtp,
                  std::shared_ptr<IMediaPacketSink> endpoint,
                  WebRtcSessionOptions options);
    ~WebRtcSession() override;

    bool ApplyRemoteOffer(const WebRtcSessionDescription& offer);
    bool ApplyRemoteOffer(const std::string& offerSdp);
    bool CreateLocalAnswer(WebRtcSessionDescription& answer);
    bool CreateLocalAnswer(std::string& answerSdp);

    bool start();
    bool stop();
    bool Tick(uint64_t nowMs);
    bool SendRtp(std::vector<uint8_t> packet);
    bool SendRtcp(std::vector<uint8_t> packet);

    WebRtcSessionState State() const noexcept { return state_.load(std::memory_order_acquire); }
    uint64_t TransportId() const noexcept { return transport_ ? transport_->Id() : 0; }
    const std::string& LastError() const noexcept { return last_error_; }

    void OnWebRtcDatagram(network::transport::DatagramProtocol protocol, network::transport::ReceivedDatagram datagram) override;
    void OnWebRtcTransportClosed() override;

private:
    void HandleStun(network::transport::ReceivedDatagram datagram);
    void HandleDtls(network::transport::ReceivedDatagram datagram);
    void HandleEncryptedRtp(network::transport::ReceivedDatagram datagram);
    void HandleEncryptedRtcp(network::transport::ReceivedDatagram datagram);
    bool BeginDtls();
    bool CompleteDtls();
    bool Fail(const std::string& error);
    bool Reject(const std::string& error);
    void Shutdown();
    bool AllowsRtp(const std::vector<uint8_t>& packet, bool sending) const;

    ice::IceAgent ice_;
    std::unique_ptr<DtlsTransport> dtls_;
    std::unique_ptr<SrtpTransport> srtp_;

    std::shared_ptr<WebRtcTransport> transport_;
    // Use media::transport::MediaEndpointIngress for an existing SfuEndpoint.
    // The application owns endpoint registration and Start/Stop independently.
    std::shared_ptr<IMediaPacketSink> endpoint_;
    WebRtcSessionOptions options_;
    WebRtcSessionDescription remote_offer_;
    WebRtcSessionDescription local_answer_;
    std::atomic<WebRtcSessionState> state_{WebRtcSessionState::New};
    std::string last_error_;
    bool dtls_started_ = false;
    bool srtp_ready_ = false;
};


}


#endif /* _WEBRTC_SESSION_H_ */
