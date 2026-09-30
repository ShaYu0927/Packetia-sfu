#include "WebRtcSession.h"
#include "WebRtcCodec.h"
#include "Sdp.h"

#include <algorithm>
#include <atomic>
#include <exception>
#include <utility>

namespace protocol::webrtc
{
namespace
{
using Protocol = network::transport::DatagramProtocol;
using Classifier = network::transport::DatagramProtocolClassifier;
using SendResult = network::transport::DatagramSendResult;

bool CanSend(MediaDirection d)
{
    return d == MediaDirection::SendRecv || d == MediaDirection::SendOnly;
}

bool CanReceive(MediaDirection d)
{
    return d == MediaDirection::SendRecv || d == MediaDirection::RecvOnly;
}

bool SameFingerprints(const DtlsParameters& a, const DtlsParameters& b)
{
    return a.fingerprints.size() == b.fingerprints.size() &&
        std::is_permutation(a.fingerprints.begin(), a.fingerprints.end(), b.fingerprints.begin(),
            [](const auto& x, const auto& y) { return x.algorithm == y.algorithm && x.value == y.value; });
}

} // namespace

WebRtcSession::WebRtcSession(std::shared_ptr<WebRtcTransport> transport,
                             std::unique_ptr<DtlsTransport> dtls,
                             std::unique_ptr<SrtpTransport> srtp,
                             std::shared_ptr<IMediaPacketSink> endpoint,
                             WebRtcSessionOptions options)
    : ice_(options.iceClock), dtls_(std::move(dtls)), srtp_(std::move(srtp)), transport_(std::move(transport)),
      endpoint_(std::move(endpoint)), options_(std::move(options))
{
}

WebRtcSession::~WebRtcSession() { Shutdown(); }

bool WebRtcSession::Reject(const std::string& error)
{
    last_error_ = error;
    return false;
}

bool WebRtcSession::Fail(const std::string& error)
{
    last_error_ = error;
    state_ = WebRtcSessionState::Failed;
    Shutdown();
    return false;
}

WebRtcSessionDescription WebRtcSession::LocalTemplate(const std::vector<WebRtcMediaDescription>& medias) const
{
    WebRtcSessionDescription local;
    local.profile = sdp::SdpProfile::WebRtc;
    local.ice = options_.ice;
    local.ice.iceLite = true;
    local.ice.options.clear();
    local.ice.endOfCandidates = true;
    local.dtls = dtls_->LocalParameters();
    local.dtls.setup = local_dtls_role_ == DtlsSetup::Unspecified ? DtlsSetup::ActPass : local_dtls_role_;
    local.origin = options_.origin;
    local.medias = medias;
    for (auto& media : local.medias)
    {
        media.ice = local.ice;
        media.dtls = local.dtls;
    }
    return local;
}

bool WebRtcSession::CheckTransport(const WebRtcSessionDescription& remote)
{
    if (remote.ice.iceLite || std::any_of(remote.medias.begin(), remote.medias.end(),
        [](const auto& media) { return (media.port || media.bundleOnly) && media.ice.iceLite; }))
        return Reject("ICE-lite requires a full ICE peer");
    if (local_dtls_role_ == DtlsSetup::Unspecified) return true;
    for (const auto& media : remote.medias)
    {
        if ((!media.port && !media.bundleOnly) || (media.media != "audio" && media.media != "video")) continue;
        const auto& ufrag = media.ice.ufrag.empty() ? remote.ice.ufrag : media.ice.ufrag;
        const auto& pwd = media.ice.pwd.empty() ? remote.ice.pwd : media.ice.pwd;
        const auto& dtls = media.dtls.fingerprints.empty() ? remote.dtls : media.dtls;
        const auto setup = media.dtls.setup == DtlsSetup::Unspecified ? remote.dtls.setup : media.dtls.setup;
        if (ufrag != remote_ice_.ufrag || pwd != remote_ice_.pwd)
            return Reject("ICE restart requires a new session");
        if (!SameFingerprints(dtls, remote_dtls_))
            return Reject("Changing DTLS identity requires a new session");
        const auto peerRole = local_dtls_role_ == DtlsSetup::Active ? DtlsSetup::Passive : DtlsSetup::Active;
        if (setup != peerRole && !(remote.type == SdpType::Offer && setup == DtlsSetup::ActPass))
            return Reject("Renegotiation must preserve the DTLS role");
    }
    return true;
}

void WebRtcSession::UpdateSignalingState()
{
    if (state_ == WebRtcSessionState::Connecting || state_ == WebRtcSessionState::Connected ||
        state_ == WebRtcSessionState::Closed || state_ == WebRtcSessionState::Failed) return;
    if (negotiation_.State() == sdp::SdpNegotiationState::HaveLocalOffer)
        state_ = WebRtcSessionState::HaveLocalOffer;
    else if (negotiation_.State() == sdp::SdpNegotiationState::HaveRemoteOffer)
        state_ = WebRtcSessionState::HaveOffer;
    else state_ = negotiation_.HasCurrent() ? WebRtcSessionState::HaveAnswer : WebRtcSessionState::New;
}

bool WebRtcSession::ApplyRemoteOffer(const std::string& offerSdp)
{
    WebRtcSessionDescription offer;
    std::string error;
    if (!sdp::Sdp::Parse(offerSdp, sdp::SdpProfile::WebRtc, SdpType::Offer, offer, error)) return Reject(error);
    return ApplyRemoteOffer(offer);
}

bool WebRtcSession::ApplyRemoteOffer(const WebRtcSessionDescription& offer)
{
    if (state_ == WebRtcSessionState::Closed || state_ == WebRtcSessionState::Failed)
        return Reject("Cannot negotiate a closed session");
    if (!CheckTransport(offer)) return false;
    if (!negotiation_.ApplyOffer(offer)) return Reject(negotiation_.LastError());
    UpdateSignalingState();
    last_error_.clear();
    return true;
}

bool WebRtcSession::CreateLocalAnswer(WebRtcSessionDescription& answer)
{
    if (state_ == WebRtcSessionState::Closed || state_ == WebRtcSessionState::Failed)
        return Reject("Cannot negotiate a closed session");
    if (negotiation_.State() == sdp::SdpNegotiationState::Stable && negotiation_.HasCurrent() &&
        negotiation_.CurrentLocal().type == SdpType::Answer)
    {
        answer = negotiation_.CurrentLocal();
        last_error_.clear();
        return true;
    }
    if (!dtls_ || !srtp_ || options_.ice.candidates.empty())
        return Reject("Configure crypto backends and local ICE candidates first");
    WebRtcSessionDescription result;
    if (!negotiation_.CreateAnswer(LocalTemplate(options_.medias), result))
        return Reject(negotiation_.LastError());
    if (!CommitNegotiation()) return false;
    answer = std::move(result);
    return true;
}

bool WebRtcSession::CreateLocalAnswer(std::string& answerSdp)
{
    WebRtcSessionDescription answer;
    if (!CreateLocalAnswer(answer)) return false;
    answerSdp = sdp::Sdp::Serialize(answer);
    return true;
}

bool WebRtcSession::CreateLocalOffer(const std::vector<WebRtcMediaDescription>& medias,
                                     WebRtcSessionDescription& offer)
{
    if (state_ == WebRtcSessionState::Closed || state_ == WebRtcSessionState::Failed)
        return Reject("Cannot negotiate a closed session");
    if (!dtls_ || !srtp_ || options_.ice.candidates.empty())
        return Reject("Configure crypto backends and local ICE candidates first");
    if (!negotiation_.CreateOffer(LocalTemplate(medias), offer)) return Reject(negotiation_.LastError());
    UpdateSignalingState();
    last_error_.clear();
    return true;
}

bool WebRtcSession::CreateLocalOffer(const std::vector<WebRtcMediaDescription>& medias, std::string& offerSdp)
{
    WebRtcSessionDescription offer;
    if (!CreateLocalOffer(medias, offer)) return false;
    offerSdp = sdp::Sdp::Serialize(offer);
    return true;
}

bool WebRtcSession::ApplyRemoteAnswer(const std::string& answerSdp)
{
    WebRtcSessionDescription answer;
    std::string error;
    if (!sdp::Sdp::Parse(answerSdp, sdp::SdpProfile::WebRtc, SdpType::Answer, answer, error)) return Reject(error);
    return ApplyRemoteAnswer(answer);
}

bool WebRtcSession::ApplyRemoteAnswer(const WebRtcSessionDescription& answer)
{
    if (state_ == WebRtcSessionState::Closed || state_ == WebRtcSessionState::Failed)
        return Reject("Cannot negotiate a closed session");
    if (!CheckTransport(answer)) return false;
    if (!negotiation_.ApplyAnswer(answer)) return Reject(negotiation_.LastError());
    return CommitNegotiation();
}

void WebRtcSession::RollbackNegotiation()
{
    negotiation_.Rollback();
    UpdateSignalingState();
    last_error_.clear();
}

bool WebRtcSession::CommitNegotiation()
{
    const auto& local = negotiation_.PendingLocal();
    const auto& remote = negotiation_.PendingRemote();
    auto bindings = payload_bindings_;
    auto capabilities = options_.medias;
    const auto reject = [&](const std::string& error)
    {
        RollbackNegotiation();
        return Reject(error);
    };
    if (!negotiation_.PendingReady()) return Reject("No completed negotiation to commit");
    if (!CheckTransport(remote))
    {
        const auto error = last_error_;
        return reject(error);
    }
    for (const auto& media : local.medias)
    {
        // Keep explicit per-MID policy from a successful business offer, while
        // retaining kind-level defaults for newly offered tracks.
        if (local.type == SdpType::Offer)
        {
            const auto previous = std::find_if(capabilities.begin(), capabilities.end(),
                [&](const auto& item) { return !item.mid.empty() && item.mid == media.mid; });
            if (previous == capabilities.end()) capabilities.push_back(media);
            else *previous = media;
        }
        if (!media.port) continue;
        if (local_dtls_role_ != DtlsSetup::Unspecified && media.dtls.setup != local_dtls_role_)
            return reject("Renegotiation must preserve the local DTLS role");
        for (const auto& codec : media.codecs)
        {
            const auto old = bindings.find(codec.payloadType);
            RtpCodecParameters compatible;
            if (old != bindings.end() && (old->second.first != media.mid ||
                !NegotiateRtpCodec(old->second.second, codec, compatible)))
                return reject("A negotiated payload type cannot change its track or RTP format");
            bindings.emplace(codec.payloadType, std::make_pair(media.mid, codec));
        }
    }
    IceParameters remoteIce = remote_ice_;
    DtlsParameters remoteDtls = remote_dtls_;
    auto localRole = local_dtls_role_;
    if (localRole == DtlsSetup::Unspecified)
    {
        const auto active = std::find_if(local.medias.begin(), local.medias.end(),
            [](const auto& media) { return media.port != 0; });
        if (active == local.medias.end()) return reject("Initial negotiation requires an active RTP transport");
        const auto index = static_cast<size_t>(active - local.medias.begin());
        remoteIce = remote.medias[index].ice;
        remoteDtls = remote.medias[index].dtls;
        localRole = active->dtls.setup;
    }
    try
    {
        if (options_.onNegotiated && !options_.onNegotiated(local, remote))
            return reject("Application rejected negotiated media; previous media retained");
    }
    catch (const std::exception&)
    {
        return reject("Application could not apply negotiated media");
    }
    // All potentially failing preparation precedes the media commit hook.
    negotiation_.Commit();
    payload_bindings_.swap(bindings);
    options_.medias.swap(capabilities);
    remote_ice_ = std::move(remoteIce);
    remote_dtls_ = std::move(remoteDtls);
    local_dtls_role_ = localRole;
    UpdateSignalingState();
    last_error_.clear();
    return true;
}

bool WebRtcSession::start()
{
    if (state_ == WebRtcSessionState::Connecting || state_ == WebRtcSessionState::Connected) return true;
    if (state_ != WebRtcSessionState::HaveAnswer) return Reject("Complete offer/answer negotiation before starting");
    if (!transport_ || !endpoint_ || weak_from_this().expired())
        return Reject("Session requires a transport, media sink and shared_ptr ownership");
    if (transport_->State() != WebRtcTransportState::Created)
        return Reject("Session requires a fresh, exclusively owned WebRTC transport");
    if (local_dtls_role_ == DtlsSetup::Unspecified) return Reject("No negotiated RTP transport");
    if (!dtls_->Configure(remote_dtls_, local_dtls_role_)) return Fail("DTLS configuration failed");
    ice_.SetRole(IceContext::Role::Controlled);
    ice_.SetLocalCredentials(options_.ice.ufrag, options_.ice.pwd);
    ice_.SetRemoteCredentials(remote_ice_.ufrag, remote_ice_.pwd);
    if (!ice_.StartLiveness(options_.iceTimeoutMs)) return Fail("ICE timeout must be positive");
    state_ = WebRtcSessionState::Connecting;
    transport_->SetSink(weak_from_this());
    if (!transport_->Start()) return Fail("Datagram transport failed to start");
    last_error_.clear();
    return true;
}

void WebRtcSession::Shutdown()
{
    if (transport_) { transport_->SetSink({}); transport_->Close(); }
    ice_.SetOnSelectedPeer({});
    ice_.Close();
    ice_.SetLocalCredentials({}, {});
    ice_.SetRemoteCredentials({}, {});
    if (dtls_) dtls_->Close();
    if (srtp_) srtp_->Close();
    dtls_started_ = false;
    srtp_ready_ = false;
}

bool WebRtcSession::stop()
{
    if (state_ == WebRtcSessionState::Closed) return true;
    state_ = WebRtcSessionState::Closed;
    Shutdown();
    return true;
}

void WebRtcSession::OnWebRtcTransportClosed() { stop(); }

bool WebRtcSession::BeginDtls()
{
    if (dtls_started_) return true;
    dtls_started_ = true;
    std::weak_ptr<WebRtcTransport> transport = transport_;
    if (!dtls_->Start([transport](const uint8_t* data, size_t size)
        {
            const auto target = transport.lock();
            return target && target->State() == WebRtcTransportState::Connected &&
                target->Send(Protocol::Dtls, data, size) == SendResult::Ok;
        })) return Fail("DTLS handshake failed to start");
    return CompleteDtls();
}

bool WebRtcSession::CompleteDtls()
{
    if (srtp_ready_ || !dtls_->IsConnected()) return true;
    SrtpKeyingMaterial keys;
    if (!dtls_->ExportSrtpKeys(keys) || keys.profile == 0 || keys.sendKey.empty() ||
        keys.receiveKey.empty() || !srtp_->Configure(keys))
        return Fail("DTLS-SRTP key installation failed");
    srtp_ready_ = true;
    state_ = WebRtcSessionState::Connected;
    return true;
}

bool WebRtcSession::Tick(uint64_t nowMs)
{
    if (state_ != WebRtcSessionState::Connecting && state_ != WebRtcSessionState::Connected) return false;
    if (!CheckIceLiveness()) return false;
    if (!dtls_started_) return true;
    if (!dtls_->Tick(nowMs)) return Fail("DTLS timeout or transport failure");
    return CompleteDtls();
}

bool WebRtcSession::CheckIceLiveness()
{
    return ice_.CheckLiveness() || Fail("ICE connectivity check timeout");
}

void WebRtcSession::OnWebRtcDatagram(Protocol protocol, network::transport::ReceivedDatagram datagram)
try
{
    if ((state_ != WebRtcSessionState::Connecting && state_ != WebRtcSessionState::Connected) ||
        !datagram.IsValid() || Classifier::Classify(datagram.Data(), datagram.Size()) != protocol) return;
    if (!CheckIceLiveness()) return;
    if (protocol != Protocol::Stun && !transport_->IsSelectedPeer(datagram.remote)) return;
    switch (protocol)
    {
    case Protocol::Stun: HandleStun(std::move(datagram)); break;
    case Protocol::Dtls: HandleDtls(std::move(datagram)); break;
    case Protocol::Rtp: HandleEncryptedRtp(std::move(datagram)); break;
    case Protocol::Rtcp: HandleEncryptedRtcp(std::move(datagram)); break;
    default: break;
    }
}

catch (const std::bad_alloc&) {} // Drop under memory pressure; keep IO alive.

void WebRtcSession::HandleStun(network::transport::ReceivedDatagram datagram)
{
    std::vector<uint8_t> response;
    const auto result = ice_.HandleDatagram(datagram.remote, datagram.Data(), datagram.Size(), response);
    if (!response.empty() && transport_->SendTo(datagram.remote, Protocol::Stun, response.data(), response.size()) != SendResult::Ok)
        return; // The peer can retransmit its connectivity check.
    if (result == ice::IceAgent::HandleResult::SuccessResponse && ice_.HasSelectedPeer())
    {
        const bool peerChanged = !transport_->IsSelectedPeer(ice_.SelectedPeer());
        if (!transport_->SelectPeer(ice_.SelectedPeer())) { Fail("ICE peer selection failed"); return; }
        if (peerChanged && options_.onSelectedPeer && !options_.onSelectedPeer(ice_.SelectedPeer()))
        {
            Fail("ICE peer address is already in use");
            return;
        }
        BeginDtls();
    }
}

void WebRtcSession::HandleDtls(network::transport::ReceivedDatagram datagram)
{
    if (!BeginDtls()) return;
    if (!dtls_->HandleDatagram(datagram.Data(), datagram.Size())) { Fail("DTLS processing failed"); return; }
    CompleteDtls();
}

bool WebRtcSession::AllowsRtp(const std::vector<uint8_t>& packet, bool sending) const
{
    if (!Classifier::IsRtp(packet.data(), packet.size())) return false;
    const int pt = packet[1] & 0x7f;
    for (const auto& media : negotiation_.CurrentLocal().medias)
        if (media.port != 0 && (sending ? CanSend(media.direction) : CanReceive(media.direction)) &&
            std::any_of(media.codecs.begin(), media.codecs.end(),
                [pt](const auto& codec) { return codec.payloadType == pt; })) return true;
    return false;
}

void WebRtcSession::HandleEncryptedRtp(network::transport::ReceivedDatagram datagram)
{
    if (!srtp_ready_) return;
    auto plaintext = datagram.payload.ToVector();
    if (!srtp_->UnprotectRtp(plaintext) || !AllowsRtp(plaintext, false)) return;
    endpoint_->OnMediaPacket(ReceivedMediaPacket(MediaPacketType::Rtp, transport_->Id(), datagram.receive_time_ms, std::move(plaintext)));
}

void WebRtcSession::HandleEncryptedRtcp(network::transport::ReceivedDatagram datagram)
{
    if (!srtp_ready_) return;
    auto plaintext = datagram.payload.ToVector();
    if (!srtp_->UnprotectRtcp(plaintext) ||
        !Classifier::IsRtcp(plaintext.data(), plaintext.size())) return;
    endpoint_->OnMediaPacket(ReceivedMediaPacket(MediaPacketType::Rtcp, transport_->Id(),datagram.receive_time_ms, std::move(plaintext)));
}

bool WebRtcSession::SendRtp(std::vector<uint8_t> packet)
{
    return state_ == WebRtcSessionState::Connected && CheckIceLiveness() && srtp_ready_ && AllowsRtp(packet, true) &&
        srtp_->ProtectRtp(packet) && transport_->Send(Protocol::Rtp, packet.data(), packet.size()) == SendResult::Ok;
}

bool WebRtcSession::SendRtcp(std::vector<uint8_t> packet)
{
    return state_ == WebRtcSessionState::Connected && CheckIceLiveness() && srtp_ready_ &&
        Classifier::IsRtcp(packet.data(), packet.size()) && srtp_->ProtectRtcp(packet) &&
        transport_->Send(Protocol::Rtcp, packet.data(), packet.size()) == SendResult::Ok;
}
} // namespace protocol::webrtc
