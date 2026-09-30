#ifndef _WEBRTC_SESSION_H_
#define _WEBRTC_SESSION_H_

#include "WebRtcTransport.h"
#include "WebRtc_Config.h"
#include "IceAgent.h"
#include "DtlsTransport.h"
#include "SrtpTransport.h"
#include "IMediaPacketSink.h"
#include "SdpNegotiator.h"

#include <memory>
#include <atomic>
#include <functional>
#include <map>

namespace protocol::webrtc
{

enum class WebRtcSessionState
{
    New, HaveOffer, HaveLocalOffer, HaveAnswer, Connecting, Connected, Closed, Failed
};

struct WebRtcSessionOptions
{
    // Supply fresh cryptographically random credentials and gathered candidates.
    IceParameters ice;
    // Capabilities match an explicit MID, then a media-kind entry with no MID.
    // H264 and Opus use codec-specific fmtp negotiation; repair codecs are excluded.
    std::vector<WebRtcMediaDescription> medias;
    sdp::SdpOrigin origin;
    uint64_t iceTimeoutMs = 30000;
    ice::IceAgent::Clock iceClock; // Optional injected monotonic clock for tests.
    // Reserve the authenticated peer in a shared UDP mux before starting DTLS.
    // Return false if another session owns this address. Runs on the owner loop.
    std::function<bool(const network::SocketAddr&)> onSelectedPeer;
    // Apply negotiated media atomically on the owner loop, before SDP commit.
    // Returning false must leave the application's previous media intact.
    std::function<bool(const WebRtcSessionDescription&, const WebRtcSessionDescription&)> onNegotiated;
};

// ICE-lite peer, one transport, RTP/RTCP mux, optional single BUNDLE group.
// The owner must serialize mutations (including transport callbacks and Tick)
// on one event loop. State() may be read by media workers. Offer/answer updates
// keep the established transport; ICE restart requires a new session.
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
    // Explicit stable MIDs; removed media are rejected and new MIDs appended.
    bool CreateLocalOffer(const std::vector<WebRtcMediaDescription>& medias, WebRtcSessionDescription& offer);
    bool CreateLocalOffer(const std::vector<WebRtcMediaDescription>& medias, std::string& offerSdp);
    bool ApplyRemoteAnswer(const WebRtcSessionDescription& answer);
    bool ApplyRemoteAnswer(const std::string& answerSdp);
    void RollbackNegotiation();
    // Owner-loop accessors; pending negotiation does not replace these views.
    const WebRtcSessionDescription& LocalDescription() const noexcept { return negotiation_.CurrentLocal(); }
    const WebRtcSessionDescription& RemoteDescription() const noexcept { return negotiation_.CurrentRemote(); }
    sdp::SdpNegotiationState SignalingState() const noexcept { return negotiation_.State(); }

    bool start();
    bool stop();
    // Owner must call periodically (e.g. every second), even without traffic.
    // nowMs goes to DTLS; ICE uses options.iceClock or steady_clock.
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
    bool CheckIceLiveness();
    WebRtcSessionDescription LocalTemplate(const std::vector<WebRtcMediaDescription>& medias) const;
    bool CheckTransport(const WebRtcSessionDescription& remote);
    bool CommitNegotiation();
    void UpdateSignalingState();

    ice::IceAgent ice_;
    std::unique_ptr<DtlsTransport> dtls_;
    std::unique_ptr<SrtpTransport> srtp_;

    std::shared_ptr<WebRtcTransport> transport_;
    // Use media::transport::MediaEndpointIngress for an existing SfuEndpoint.
    // The application owns endpoint registration and Start/Stop independently.
    std::shared_ptr<IMediaPacketSink> endpoint_;
    WebRtcSessionOptions options_;
    sdp::SdpNegotiator negotiation_;
    IceParameters remote_ice_;
    DtlsParameters remote_dtls_;
    DtlsSetup local_dtls_role_ = DtlsSetup::Unspecified;
    // A PT cannot be reassigned to another MID or RTP format during a session.
    std::map<int, std::pair<std::string, RtpCodecParameters>> payload_bindings_;
    std::atomic<WebRtcSessionState> state_{WebRtcSessionState::New};
    std::string last_error_;
    bool dtls_started_ = false;
    bool srtp_ready_ = false;
};


}


#endif /* _WEBRTC_SESSION_H_ */
