#include "WebRtcSession.h"
#include "WebRtcCodec.h"
#include "Sdp.h"

#include <algorithm>
#include <atomic>
#include <charconv>
#include <map>
#include <set>
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

MediaDirection AnswerDirection(MediaDirection remote, MediaDirection local)
{
    const bool send = CanSend(local) && CanReceive(remote);
    const bool receive = CanReceive(local) && CanSend(remote);
    return send ? (receive ? MediaDirection::SendRecv : MediaDirection::SendOnly)
                : (receive ? MediaDirection::RecvOnly : MediaDirection::Inactive);
}

bool HasMid(const BundleParameters& bundle, const std::string& mid)
{
    return std::find(bundle.mids.begin(), bundle.mids.end(), mid) != bundle.mids.end();
}

bool IsOffered(const WebRtcMediaDescription& media)
{
    return media.port != 0 || media.bundleOnly;
}

bool ValidCredentials(const IceParameters& ice)
{
    auto valid = [](const std::string& value, size_t minimum)
    {
        return value.size() >= minimum && value.size() <= 256 &&
            value.find_first_not_of("abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789+/") == std::string::npos;
    };
    return valid(ice.ufrag, 4) && valid(ice.pwd, 22);
}

bool ValidDtls(const DtlsParameters& dtls)
{
    return !dtls.fingerprints.empty() &&
        std::all_of(dtls.fingerprints.begin(), dtls.fingerprints.end(), [](const auto& fp)
        { return !fp.algorithm.empty() && !fp.value.empty(); });
}

bool SameDtls(const DtlsParameters& a, const DtlsParameters& b)
{
    return a.setup == b.setup && a.fingerprints.size() == b.fingerprints.size() &&
        std::is_permutation(a.fingerprints.begin(), a.fingerprints.end(), b.fingerprints.begin(),
            [](const auto& x, const auto& y) { return x.algorithm == y.algorithm && x.value == y.value; });
}

bool SameFeedback(const RtcpFeedback& a, const RtcpFeedback& b)
{
    return a.type == b.type && a.parameter == b.parameter;
}

std::vector<RtcpFeedback> EffectiveFeedback(const std::vector<RtcpFeedback>& common,
                                           const std::vector<RtcpFeedback>& codec)
{
    auto result = common;
    for (const auto& item : codec)
        if (std::none_of(result.begin(), result.end(),
                [&](const auto& existing) { return SameFeedback(item, existing); }))
            result.push_back(item);
    return result;
}

std::vector<RtcpFeedback> FeedbackIntersection(const std::vector<RtcpFeedback>& remote,
                                               const std::vector<RtcpFeedback>& local)
{
    std::vector<RtcpFeedback> result;
    for (const auto& feedback : remote)
        if (std::any_of(local.begin(), local.end(), [&](const auto& supported)
                { return SameFeedback(feedback, supported); }) &&
            std::none_of(result.begin(), result.end(), [&](const auto& existing)
                { return SameFeedback(feedback, existing); }))
            result.push_back(feedback);
    return result;
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

bool WebRtcSession::ApplyRemoteOffer(const std::string& offerSdp)
{
    WebRtcSessionDescription offer;
    std::string error;
    if (!sdp::Sdp::Parse(offerSdp, sdp::SdpProfile::WebRtc, SdpType::Offer, offer, error)) return Reject(error);
    return ApplyRemoteOffer(offer);
}

bool WebRtcSession::CreateLocalAnswer(std::string& answerSdp)
{
    WebRtcSessionDescription answer;
    if (!CreateLocalAnswer(answer)) return false;
    answerSdp = sdp::Sdp::Serialize(answer);
    return true;
}

bool WebRtcSession::ApplyRemoteOffer(const WebRtcSessionDescription& offer)
{
    if (state_ != WebRtcSessionState::New && state_ != WebRtcSessionState::HaveOffer)
        return Reject("Renegotiation is not supported; create a new session");
    if (offer.type != SdpType::Offer || offer.medias.empty())
        return Reject("Expected an offer with normalized media descriptions");

    auto normalized = offer;
    std::set<std::string> mids;
    const WebRtcMediaDescription* transportMedia = nullptr;
    size_t active = 0;
    for (auto& media : normalized.medias)
    {
        if (media.mid.empty() || !mids.insert(media.mid).second || media.fmts.empty() ||
            media.port < 0 || media.port > 65535 || media.portCount != 1)
            return Reject("Invalid or duplicate mid, media port or formats");
        if (media.bundleOnly && (!HasMid(offer.bundle, media.mid) || media.port != 0))
            return Reject("Invalid bundle-only media");
        if (!IsOffered(media)) continue;
        if (media.media != "audio" && media.media != "video") continue;
        if (media.proto != "UDP/TLS/RTP/SAVPF" || !media.rtcpMux)
            return Reject("Only UDP DTLS-SRTP with rtcp-mux is supported");
        if (!offer.bundle.mids.empty() && !HasMid(offer.bundle, media.mid))
            return Reject("All active RTP media must share the BUNDLE transport");
        if (media.ice.ufrag.empty()) media.ice.ufrag = offer.ice.ufrag;
        if (media.ice.pwd.empty()) media.ice.pwd = offer.ice.pwd;
        if (media.dtls.fingerprints.empty()) media.dtls.fingerprints = offer.dtls.fingerprints;
        if (media.dtls.setup == DtlsSetup::Unspecified) media.dtls.setup = offer.dtls.setup;
        if (offer.ice.iceLite || media.ice.iceLite || !ValidCredentials(media.ice))
            return Reject("ICE-lite requires a full ICE peer with valid credentials");
        if (!ValidDtls(media.dtls) || (media.dtls.setup != DtlsSetup::ActPass &&
            media.dtls.setup != DtlsSetup::Active && media.dtls.setup != DtlsSetup::Passive))
            return Reject("Missing fingerprint or unsupported DTLS setup role");
        if (transportMedia && (media.ice.ufrag != transportMedia->ice.ufrag ||
            media.ice.pwd != transportMedia->ice.pwd || !SameDtls(media.dtls, transportMedia->dtls)))
            return Reject("This answerer requires identical transport parameters across BUNDLE media");
        transportMedia = &media;
        ++active;
        std::set<int> formats;
        for (const auto& format : media.fmts)
        {
            int pt = -1;
            const auto parsed = std::from_chars(format.data(), format.data() + format.size(), pt);
            if (parsed.ec != std::errc{} || parsed.ptr != format.data() + format.size() ||
                pt < 0 || pt > 127 || (pt >= 64 && pt <= 95) ||
                format != std::to_string(pt) || !formats.insert(pt).second)
                return Reject("Invalid or duplicate RTP format in m= line");
        }
        std::set<int> pts;
        for (const auto& codec : media.codecs)
            if (codec.payloadType < 0 || codec.payloadType > 127 ||
                (codec.payloadType >= 64 && codec.payloadType <= 95) || codec.encodingName.empty() ||
                codec.clockRate <= 0 || codec.channels < 0 || !pts.insert(codec.payloadType).second ||
                std::find(media.fmts.begin(), media.fmts.end(), std::to_string(codec.payloadType)) == media.fmts.end())
                return Reject("Invalid RTP codec or payload type");
        if (pts != formats) return Reject("Every offered RTP format needs a codec description");
        std::set<int> extensionIds;
        for (const auto& extension : media.headerExtensions)
            if (extension.id < 1 || extension.id > 255 || extension.uri.empty() ||
                !extensionIds.insert(extension.id).second)
                return Reject("Invalid or duplicate RTP header extension");
    }
    std::set<std::string> bundleMids;
    for (const auto& mid : offer.bundle.mids)
        if (!mids.count(mid) || !bundleMids.insert(mid).second)
            return Reject("BUNDLE references an unknown or duplicate mid");
    if (active == 0 || (active > 1 && offer.bundle.mids.empty()))
        return Reject("Expected one RTP transport (use BUNDLE for multiple media)");
    remote_offer_ = std::move(normalized);
    state_ = WebRtcSessionState::HaveOffer;
    last_error_.clear();
    return true;
}

bool WebRtcSession::CreateLocalAnswer(WebRtcSessionDescription& answer)
{
    if (state_ == WebRtcSessionState::HaveAnswer)
    {
        answer = local_answer_;
        last_error_.clear();
        return true;
    }
    if (state_ != WebRtcSessionState::HaveOffer) return Reject("Apply an offer before creating an answer");
    if (!dtls_ || !srtp_ || !ValidCredentials(options_.ice) || options_.ice.candidates.empty())
        return Reject("Configure crypto backends, local ICE credentials and candidates first");
    std::set<std::string> capabilityKinds;
    for (const auto& capability : options_.medias)
        if (!capabilityKinds.insert(capability.media).second)
            return Reject("Configure one local capability entry per media kind");
    const auto identity = dtls_->LocalParameters();
    if (!ValidDtls(identity)) return Reject("DTLS backend has no certificate fingerprint");

    WebRtcSessionDescription result;
    result.type = SdpType::Answer;
    result.profile = sdp::SdpProfile::WebRtc;
    result.ice = options_.ice;
    result.ice.iceLite = true;
    result.ice.options.clear(); // This version does not implement trickle signaling.
    result.ice.endOfCandidates = true;
    result.origin = options_.origin;
    if (result.origin.sess_id.empty())
    {
        static std::atomic<uint64_t> nextId{1};
        result.origin.sess_id = std::to_string(nextId.fetch_add(1));
    }
    if (result.origin.username.empty()) result.origin.username = "-";
    if (result.origin.sess_version.empty()) result.origin.sess_version = "0";
    if (result.origin.net_type.empty()) result.origin.net_type = "IN";
    if (result.origin.addr_type.empty()) result.origin.addr_type = "IP4";
    if (result.origin.unicast_address.empty()) result.origin.unicast_address = "0.0.0.0";
    result.session_name = "-";
    result.timing = "0 0";
    result.connection = "IN IP4 0.0.0.0";
    result.conn = {"IN", "IP4", "0.0.0.0"};
    size_t accepted = 0;
    for (const auto& remote : remote_offer_.medias)
    {
        WebRtcMediaDescription local;
        local.mid = remote.mid;
        local.media = remote.media;
        local.proto = remote.proto;
        local.fmts = remote.fmts;
        local.direction = MediaDirection::Inactive;
        const auto capability = std::find_if(options_.medias.begin(), options_.medias.end(),
            [&](const auto& item) { return item.media == remote.media; });
        if (IsOffered(remote) && (remote.media == "audio" || remote.media == "video") &&
            capability != options_.medias.end())
        {
            // m= order expresses the offerer's codec preference; the typed
            // codec vector and a=rtpmap lines need not have that same order.
            for (const auto& format : remote.fmts)
            {
                const auto offered = std::find_if(remote.codecs.begin(), remote.codecs.end(),
                    [&](const auto& codec) { return std::to_string(codec.payloadType) == format; });
                if (offered == remote.codecs.end()) continue;
                for (const auto& supported : capability->codecs)
                {
                    RtpCodecParameters negotiated;
                    if (!NegotiateRtpCodec(*offered, supported, negotiated)) continue;
                    negotiated.rtcpFeedback = FeedbackIntersection(
                        EffectiveFeedback(remote.rtcpFeedback, offered->rtcpFeedback),
                        EffectiveFeedback(capability->rtcpFeedback, supported.rtcpFeedback));
                    local.codecs.push_back(std::move(negotiated));
                    break;
                }
            }
            if (!local.codecs.empty())
            {
                local.port = 9;
                local.conn = {"IN", "IP4", "0.0.0.0"};
                local.fmts.clear();
                for (const auto& codec : local.codecs) local.fmts.push_back(std::to_string(codec.payloadType));
                local.direction = AnswerDirection(remote.direction, capability->direction);
                local.ice = result.ice;
                local.dtls = identity;
                local.dtls.setup = remote.dtls.setup == DtlsSetup::Active ? DtlsSetup::Passive : DtlsSetup::Active;
                result.dtls = local.dtls;
                local.rtcpMux = true;
                local.rtcpRsize = remote.rtcpRsize && capability->rtcpRsize;
                std::set<int> extensionIds;
                for (const auto& extension : remote.headerExtensions)
                {
                    if (extension.id < 1 || extension.id > 14 || !extensionIds.insert(extension.id).second) continue;
                    const auto supported = std::find_if(capability->headerExtensions.begin(), capability->headerExtensions.end(),
                        [&](const auto& item) { return item.uri == extension.uri && item.attributes == extension.attributes; });
                    if (supported == capability->headerExtensions.end()) continue;
                    auto negotiated = extension;
                    negotiated.direction = AnswerDirection(extension.direction, supported->direction);
                    const bool send = CanSend(negotiated.direction) && CanSend(local.direction);
                    const bool receive = CanReceive(negotiated.direction) && CanReceive(local.direction);
                    negotiated.direction = send ? (receive ? MediaDirection::SendRecv : MediaDirection::SendOnly)
                                                : (receive ? MediaDirection::RecvOnly : MediaDirection::Inactive);
                    if (negotiated.direction != MediaDirection::Inactive) local.headerExtensions.push_back(std::move(negotiated));
                }
                const bool transportCc = std::any_of(local.headerExtensions.begin(), local.headerExtensions.end(),
                    [](const auto& extension) { return extension.uri == RtpHeaderExtensionUri::TRANSPORT_CC; });
                if (!transportCc)
                    for (auto& codec : local.codecs)
                        codec.rtcpFeedback.erase(std::remove_if(codec.rtcpFeedback.begin(), codec.rtcpFeedback.end(),
                            [](const auto& item) { return item.type == RtcpFeedbackType::TRANSPORT_CC; }), codec.rtcpFeedback.end());
                if (CanSend(local.direction))
                {
                    local.ssrcs = capability->ssrcs;
                    local.ssrcGroups = capability->ssrcGroups;
                    local.msids = capability->msids;
                }
                ++accepted;
            }
        }
        result.medias.push_back(std::move(local));
    }
    if (accepted == 0) return Reject("No compatible RTP codecs in the offer");
    // Ambiguous PTs need MID/SSRC demultiplexing, not implemented here.
    std::set<int> bundledPts;
    std::map<int, std::string> bundledExtensions;
    std::set<uint32_t> bundledSsrcs;
    for (const auto& media : result.medias)
    {
        for (const auto& codec : media.codecs)
            if (!bundledPts.insert(codec.payloadType).second)
                return Reject("Ambiguous bundled payload types require MID demultiplexing");
        for (const auto& extension : media.headerExtensions)
        {
            const auto found = bundledExtensions.emplace(extension.id, extension.uri);
            if (!found.second && found.first->second != extension.uri)
                return Reject("BUNDLE header extension IDs must identify the same URI");
        }
        for (const auto& source : media.ssrcs)
            if (!bundledSsrcs.insert(source.ssrc).second)
                return Reject("Local SSRCs must be unique across accepted BUNDLE media");
    }
    for (const auto& mid : remote_offer_.bundle.mids)
        if (std::any_of(result.medias.begin(), result.medias.end(),
            [&](const auto& media) { return media.mid == mid && media.port != 0; }))
            result.bundle.mids.push_back(mid);
    local_answer_ = std::move(result);
    answer = local_answer_;
    state_ = WebRtcSessionState::HaveAnswer;
    last_error_.clear();
    return true;
}

bool WebRtcSession::start()
{
    if (state_ == WebRtcSessionState::Connecting || state_ == WebRtcSessionState::Connected) return true;
    if (state_ != WebRtcSessionState::HaveAnswer) return Reject("Create the local answer before starting");
    if (!transport_ || !endpoint_ || weak_from_this().expired())
        return Reject("Session requires a transport, media sink and shared_ptr ownership");
    if (transport_->State() != WebRtcTransportState::Created)
        return Reject("Session requires a fresh, exclusively owned WebRTC transport");
    const auto selected = std::find_if(local_answer_.medias.begin(), local_answer_.medias.end(),
        [](const auto& media) { return media.port != 0; });
    const auto index = static_cast<size_t>(selected - local_answer_.medias.begin());
    const auto& remote = remote_offer_.medias[index];
    if (!dtls_->Configure(remote.dtls, selected->dtls.setup)) return Fail("DTLS configuration failed");
    ice_.SetRole(IceContext::Role::Controlled);
    ice_.SetLocalCredentials(local_answer_.ice.ufrag, local_answer_.ice.pwd);
    ice_.SetRemoteCredentials(remote.ice.ufrag, remote.ice.pwd);
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
        if (!transport_->SelectPeer(ice_.SelectedPeer())) { Fail("ICE peer selection failed"); return; }
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
    for (const auto& media : local_answer_.medias)
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
    endpoint_->OnMediaPacket(ReceivedMediaPacket(MediaPacketType::Rtcp, transport_->Id(),
        datagram.receive_time_ms, std::move(plaintext)));
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
