#include "WebRtcSession.h"
#include "WebRtcCodec.h"
#include "Sdp.h"
#include "CryptoUtil.h"
#include "../../Rtsp/Rtp/RtpHeaderExtensions.h"

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

// Compare effective negotiated media while ignoring ICE generation and network addresses.
std::string MediaShape(WebRtcSessionDescription description)
{
    description.origin = {};
    description.ice = {};
    description.dtls = {};
    description.conn = {};
    description.connection.clear();
    for (auto& media : description.medias)
    {
        media.ice = {};
        media.dtls = {};
        media.conn = {};
        if (media.port) media.port = 9;
        media.attributes.erase(std::remove_if(media.attributes.begin(), media.attributes.end(),
            [](const auto& attribute) { return attribute.key == "rtcp"; }), media.attributes.end());
    }
    return sdp::Sdp::Serialize(description);
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
        if (!restarting_ice_ && (ufrag != remote_ice_.ufrag || pwd != remote_ice_.pwd))
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
    if (!negotiation_.CreateAnswer(LocalTemplate(options_.medias), result, options_.singleCodecPerMedia))
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

bool WebRtcSession::RestartIce(const std::string& offerSdp, WebRtcSessionDescription& answer)
{
    WebRtcSessionDescription offer;
    std::string error;
    if (!sdp::Sdp::Parse(offerSdp, sdp::SdpProfile::WebRtc, SdpType::Offer, offer, error)) return Reject(error);
    return RestartIce(offer, answer);
}

bool WebRtcSession::RestartIce(const WebRtcSessionDescription& offer, WebRtcSessionDescription& answer)
{
    if ((state_ != WebRtcSessionState::Connecting && state_ != WebRtcSessionState::Connected) ||
        negotiation_.State() != sdp::SdpNegotiationState::Stable || restarting_ice_)
        return Reject("ICE restart requires a live session with stable negotiation");
    for (const auto& media : offer.medias)
        if ((media.port || media.bundleOnly) &&
            ((media.ice.ufrag.empty() ? offer.ice.ufrag : media.ice.ufrag) == remote_ice_.ufrag ||
             (media.ice.pwd.empty() ? offer.ice.pwd : media.ice.pwd) == remote_ice_.pwd))
            return Reject("ICE restart requires fresh remote ufrag and password");

    auto local = LocalTemplate(negotiation_.CurrentLocal().medias);
    if (!utils::SecureRandomHex(8, local.ice.ufrag) || !utils::SecureRandomHex(24, local.ice.pwd))
        return Reject("Could not generate ICE restart credentials");
    for (auto& media : local.medias) media.ice = local.ice;
    restarting_ice_ = true;
    try
    {
        WebRtcSessionDescription result;
        if (!ApplyRemoteOffer(offer) || !negotiation_.CreateAnswer(local, result, options_.singleCodecPerMedia))
        {
            const auto reason = last_error_.empty() ? negotiation_.LastError() : last_error_;
            RollbackNegotiation();
            restarting_ice_ = false;
            return Reject(reason);
        }
        if (MediaShape(negotiation_.PendingRemote()) != MediaShape(negotiation_.CurrentRemote()) ||
            MediaShape(result) != MediaShape(negotiation_.CurrentLocal()))
        {
            RollbackNegotiation();
            restarting_ice_ = false;
            return Reject("ICE restart must preserve the published and subscribed media");
        }
        if (!CommitNegotiation())
        {
            const auto reason = last_error_;
            restarting_ice_ = false;
            return Reject(reason);
        }
        restarting_ice_ = false;
        answer = std::move(result);
        return true;
    }
    catch (...)
    {
        restarting_ice_ = false;
        RollbackNegotiation();
        throw;
    }
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
            const auto key = std::make_pair(media.mid, codec.payloadType);
            const auto old = bindings.find(key);
            RtpCodecParameters compatible;
            if (old != bindings.end() && !NegotiateRtpCodec(old->second, codec, compatible))
                return reject("A negotiated payload type cannot change its RTP format within a MID");
            bindings.emplace(key, codec);
        }
    }
    IceParameters remoteIce = remote_ice_;
    DtlsParameters remoteDtls = remote_dtls_;
    auto localRole = local_dtls_role_;
    if (localRole == DtlsSetup::Unspecified || restarting_ice_)
    {
        const auto active = std::find_if(local.medias.begin(), local.medias.end(),
            [](const auto& media) { return media.port != 0; });
        if (active == local.medias.end()) return reject("Initial negotiation requires an active RTP transport");
        const auto index = static_cast<size_t>(active - local.medias.begin());
        remoteIce = remote.medias[index].ice;
        remoteDtls = remote.medias[index].dtls;
        localRole = active->dtls.setup;
    }
    IceParameters localIce = local.ice;
    auto localUfrag = localIce.ufrag, localPwd = localIce.pwd;
    auto remoteUfrag = remoteIce.ufrag, remotePwd = remoteIce.pwd;
    try
    {
        if (!restarting_ice_ && options_.onNegotiated && !options_.onNegotiated(local, remote))
            return reject("Application rejected negotiated media; previous media retained");
        if (restarting_ice_ && options_.onIceRestart && !options_.onIceRestart(options_.ice.ufrag, localIce.ufrag))
            return reject("Could not replace ICE routing generation");
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
    if (restarting_ice_)
    {
        options_.ice = std::move(localIce);
        ice_.SetLocalCredentials(std::move(localUfrag), std::move(localPwd));
        ice_.SetRemoteCredentials(std::move(remoteUfrag), std::move(remotePwd));
        ice_.StartLiveness(options_.iceTimeoutMs);
        transport_->ClearSelectedPeer();
        awaiting_restart_nomination_ = true;
        state_ = WebRtcSessionState::Connecting;
    }
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
    awaiting_restart_nomination_ = false;
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
    if (!dtls_started_ || awaiting_restart_nomination_) return true;
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
        awaiting_restart_nomination_ = false;
        BeginDtls();
        if (srtp_ready_) state_ = WebRtcSessionState::Connected;
    }
}

void WebRtcSession::HandleDtls(network::transport::ReceivedDatagram datagram)
{
    if (!BeginDtls()) return;
    if (!dtls_->HandleDatagram(datagram.Data(), datagram.Size())) { Fail("DTLS processing failed"); return; }
    CompleteDtls();
}

bool WebRtcSession::AllowsRtp(const std::vector<uint8_t>& packet, bool sending)
{
    if (!Classifier::IsRtp(packet.data(), packet.size())) return false;
    std::unordered_map<uint8_t, std::string> extensions;
    if (!rtsp::ReadRtpHeaderExtensions(packet.data(), packet.size(), extensions)) return false;
    const int pt = packet[1] & 0x7f;
    const uint32_t ssrc = (uint32_t(packet[8]) << 24) | (uint32_t(packet[9]) << 16) |
        (uint32_t(packet[10]) << 8) | packet[11];
    auto& bindings = sending ? send_ssrc_bindings_ : receive_ssrc_bindings_;
    const auto bound = bindings.find(ssrc);
    std::string selected = bound == bindings.end() ? std::string{} : bound->second;
    const auto select = [&](const std::string& mid)
    {
        if (!selected.empty() && selected != mid) return false;
        selected = mid;
        return true;
    };
    bool hasMid = false, matchedMid = false;
    for (const auto& media : negotiation_.CurrentLocal().medias)
        for (const auto& extension : media.headerExtensions)
        {
            if (extension.uri != "urn:ietf:params:rtp-hdrext:sdes:mid" ||
                !(sending ? CanSend(extension.direction) : CanReceive(extension.direction))) continue;
            const auto value = extensions.find(static_cast<uint8_t>(extension.id));
            if (value == extensions.end()) continue;
            hasMid = true;
            if (value->second != media.mid) continue;
            if (!select(media.mid)) return false;
            matchedMid = true;
        }
    if (hasMid && !matchedMid) return false;
    const auto& sources = sending ? negotiation_.CurrentLocal() : negotiation_.CurrentRemote();
    for (const auto& media : sources.medias)
        for (const auto& source : media.ssrcs)
            if (source.ssrc == ssrc && !select(media.mid)) return false;
    if (selected.empty())
        for (const auto& media : negotiation_.CurrentLocal().medias)
            if (media.port != 0 && std::any_of(media.codecs.begin(), media.codecs.end(),
                [pt](const auto& codec) { return codec.payloadType == pt; }))
            {
                if (!selected.empty()) return false;
                selected = media.mid;
            }
    for (const auto& media : negotiation_.CurrentLocal().medias)
        if (media.mid == selected && media.port != 0 && (sending ? CanSend(media.direction) : CanReceive(media.direction)) &&
            std::any_of(media.codecs.begin(), media.codecs.end(),
                [pt](const auto& codec) { return codec.payloadType == pt; }))
        {
            bindings.emplace(ssrc, media.mid);
            return true;
        }
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
