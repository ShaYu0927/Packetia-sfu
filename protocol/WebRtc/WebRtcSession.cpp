#include "WebRtcSession.h"

#include <algorithm>
#include <atomic>
#include <cctype>
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

const char* DirectionName(MediaDirection d)
{
    switch (d)
    {
    case MediaDirection::SendRecv: return "sendrecv";
    case MediaDirection::SendOnly: return "sendonly";
    case MediaDirection::RecvOnly: return "recvonly";
    default: return "inactive";
    }
}

bool HasMid(const BundleParameters& bundle, const std::string& mid)
{
    return std::find(bundle.mids.begin(), bundle.mids.end(), mid) != bundle.mids.end();
}

bool IsOffered(const WebRtcMediaDescription& media)
{
    return media.sdp.port != 0 || media.bundleOnly;
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
        std::equal(a.fingerprints.begin(), a.fingerprints.end(), b.fingerprints.begin(),
            [](const auto& x, const auto& y) { return x.algorithm == y.algorithm && x.value == y.value; });
}

std::string Lower(std::string value)
{
    for (auto& c : value) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return value;
}

bool SameCodec(const RtpCodecParameters& a, const RtpCodecParameters& b)
{
    return Lower(a.encodingName) == Lower(b.encodingName) && a.clockRate == b.clockRate &&
        (std::max)(1, a.channels) == (std::max)(1, b.channels) && a.fmtp == b.fmtp;
}

std::vector<RtcpFeedback> FeedbackIntersection(const std::vector<RtcpFeedback>& remote,
                                               const std::vector<RtcpFeedback>& local)
{
    std::vector<RtcpFeedback> result;
    for (const auto& feedback : remote)
        if (std::any_of(local.begin(), local.end(), [&](const auto& supported)
            { return feedback.type == supported.type && feedback.parameter == supported.parameter; }))
            result.push_back(feedback);
    return result;
}

void AddFeedback(sdp::SdpMedia& media, const std::string& pt, const std::vector<RtcpFeedback>& feedback)
{
    for (const auto& item : feedback)
        media.attributes.push_back({"rtcp-fb", pt + " " + item.type +
            (item.parameter.empty() ? "" : " " + item.parameter)});
}

// Construct raw SDP from negotiated values; do not copy remote transport,
// SSRC or unknown attributes into the local answer.
void BuildMediaSdp(WebRtcMediaDescription& media)
{
    auto& raw = media.sdp;
    raw.attributes = {{"mid", media.mid}, {DirectionName(media.direction), ""}};
    if (raw.port == 0) return;
    raw.conn = {"IN", "IP4", "0.0.0.0"};
    raw.fmts.clear();
    raw.attributes.push_back({"rtcp-mux", ""});
    if (media.rtcpRsize) raw.attributes.push_back({"rtcp-rsize", ""});
    raw.attributes.push_back({"ice-ufrag", media.ice.ufrag});
    raw.attributes.push_back({"ice-pwd", media.ice.pwd});
    for (const auto& candidate : media.ice.candidates) raw.attributes.push_back({"candidate", candidate});
    if (media.ice.endOfCandidates) raw.attributes.push_back({"end-of-candidates", ""});
    raw.attributes.push_back({"setup", media.dtls.setup == DtlsSetup::Active ? "active" : "passive"});
    for (const auto& fp : media.dtls.fingerprints) raw.attributes.push_back({"fingerprint", fp.algorithm + " " + fp.value});
    for (const auto& codec : media.codecs)
    {
        const auto pt = std::to_string(codec.payloadType);
        raw.fmts.push_back(pt);
        raw.rtpmaps.push_back({codec.payloadType, codec.encodingName, codec.clockRate, codec.channels});
        raw.attributes.push_back({"rtpmap", pt + " " + codec.encodingName + "/" +
            std::to_string(codec.clockRate) + (codec.channels > 1 ? "/" + std::to_string(codec.channels) : "")});
        if (!codec.fmtp.empty())
        {
            raw.fmtps.push_back({codec.payloadType, codec.fmtp});
            raw.attributes.push_back({"fmtp", pt + " " + codec.fmtp});
        }
        AddFeedback(raw, pt, codec.rtcpFeedback);
    }
    AddFeedback(raw, "*", media.rtcpFeedback);
    for (const auto& extension : media.headerExtensions)
        raw.attributes.push_back({"extmap", std::to_string(extension.id) + "/" +
            DirectionName(extension.direction) + " " + extension.uri +
            (extension.attributes.empty() ? "" : " " + extension.attributes)});
    for (const auto& msid : media.msids) raw.attributes.push_back({"msid", msid});
    for (const auto& source : media.ssrcs)
        for (const auto& attr : source.attributes)
            raw.attributes.push_back({"ssrc", std::to_string(source.ssrc) + " " + attr.key +
                (attr.value.empty() ? "" : ":" + attr.value)});
    for (const auto& group : media.ssrcGroups)
    {
        std::string value = group.semantics;
        for (auto ssrc : group.ssrcs) value += " " + std::to_string(ssrc);
        raw.attributes.push_back({"ssrc-group", std::move(value)});
    }
}
} // namespace

WebRtcSession::WebRtcSession(std::shared_ptr<WebRtcTransport> transport,
                             std::unique_ptr<DtlsTransport> dtls,
                             std::unique_ptr<SrtpTransport> srtp,
                             std::shared_ptr<IMediaPacketSink> endpoint,
                             WebRtcSessionOptions options)
    : dtls_(std::move(dtls)), srtp_(std::move(srtp)), transport_(std::move(transport)),
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
        if (media.mid.empty() || !mids.insert(media.mid).second || media.sdp.fmts.empty() ||
            media.sdp.port < 0 || media.sdp.port > 65535)
            return Reject("Invalid or duplicate mid, media port or formats");
        if (media.bundleOnly && (!HasMid(offer.bundle, media.mid) || media.sdp.port != 0))
            return Reject("Invalid bundle-only media");
        if (!IsOffered(media)) continue;
        if (media.sdp.media != "audio" && media.sdp.media != "video") continue;
        if (media.sdp.proto != "UDP/TLS/RTP/SAVPF" || !media.rtcpMux)
            return Reject("Only UDP DTLS-SRTP with rtcp-mux is supported");
        if (!offer.bundle.mids.empty() && !HasMid(offer.bundle, media.mid))
            return Reject("All active RTP media must share the BUNDLE transport");
        if (media.ice.ufrag.empty() && media.ice.pwd.empty()) media.ice = offer.ice;
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
        std::set<int> pts;
        for (const auto& codec : media.codecs)
            if (codec.payloadType < 0 || codec.payloadType > 127 ||
                (codec.payloadType >= 64 && codec.payloadType <= 95) || codec.encodingName.empty() ||
                codec.clockRate <= 0 || codec.channels < 0 || !pts.insert(codec.payloadType).second ||
                std::find(media.sdp.fmts.begin(), media.sdp.fmts.end(), std::to_string(codec.payloadType)) == media.sdp.fmts.end())
                return Reject("Invalid RTP codec or payload type");
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
    if (state_ == WebRtcSessionState::HaveAnswer) { answer = local_answer_; return true; }
    if (state_ != WebRtcSessionState::HaveOffer) return Reject("Apply an offer before creating an answer");
    if (!dtls_ || !srtp_ || !ValidCredentials(options_.ice) || options_.ice.candidates.empty())
        return Reject("Configure crypto backends, local ICE credentials and candidates first");
    const auto identity = dtls_->LocalParameters();
    if (!ValidDtls(identity)) return Reject("DTLS backend has no certificate fingerprint");

    WebRtcSessionDescription result;
    result.type = SdpType::Answer;
    result.ice = options_.ice;
    result.ice.iceLite = true;
    result.ice.options.clear(); // This version does not implement trickle signaling.
    result.sdp.origin = options_.origin;
    if (result.sdp.origin.sess_id.empty())
    {
        static std::atomic<uint64_t> nextId{1};
        result.sdp.origin.sess_id = std::to_string(nextId.fetch_add(1));
    }
    if (result.sdp.origin.username.empty()) result.sdp.origin.username = "-";
    if (result.sdp.origin.sess_version.empty()) result.sdp.origin.sess_version = "0";
    if (result.sdp.origin.net_type.empty()) result.sdp.origin.net_type = "IN";
    if (result.sdp.origin.addr_type.empty()) result.sdp.origin.addr_type = "IP4";
    if (result.sdp.origin.unicast_address.empty()) result.sdp.origin.unicast_address = "0.0.0.0";
    result.sdp.session_name = "-";
    result.sdp.timing = "0 0";
    result.sdp.connection = "IN IP4 0.0.0.0";
    result.sdp.conn = {"IN", "IP4", "0.0.0.0"};
    result.sdp.attributes.push_back({"ice-lite", ""});
    size_t accepted = 0;
    for (const auto& remote : remote_offer_.medias)
    {
        WebRtcMediaDescription local;
        local.mid = remote.mid;
        local.sdp.media = remote.sdp.media;
        local.sdp.proto = remote.sdp.proto;
        local.sdp.fmts = remote.sdp.fmts;
        local.direction = MediaDirection::Inactive;
        const auto capability = std::find_if(options_.medias.begin(), options_.medias.end(),
            [&](const auto& item) { return item.sdp.media == remote.sdp.media; });
        if (IsOffered(remote) && (remote.sdp.media == "audio" || remote.sdp.media == "video") &&
            capability != options_.medias.end())
        {
            for (const auto& codec : remote.codecs)
            {
                // These codecs require dependency negotiation and PT remapping.
                const auto name = Lower(codec.encodingName);
                if (name == "rtx" || name == "red" || name == "ulpfec" || name == "flexfec-03") continue;
                const auto supported = std::find_if(capability->codecs.begin(), capability->codecs.end(),
                    [&](const auto& item) { return SameCodec(codec, item); });
                if (supported == capability->codecs.end()) continue;
                auto negotiated = codec;
                negotiated.rtcpFeedback = FeedbackIntersection(codec.rtcpFeedback, supported->rtcpFeedback);
                local.codecs.push_back(std::move(negotiated));
            }
            if (!local.codecs.empty())
            {
                local.sdp.port = 9;
                local.direction = AnswerDirection(remote.direction, capability->direction);
                local.ice = result.ice;
                local.dtls = identity;
                local.dtls.setup = remote.dtls.setup == DtlsSetup::Active ? DtlsSetup::Passive : DtlsSetup::Active;
                result.dtls = local.dtls;
                local.rtcpMux = true;
                local.rtcpRsize = remote.rtcpRsize && capability->rtcpRsize;
                local.rtcpFeedback = FeedbackIntersection(remote.rtcpFeedback, capability->rtcpFeedback);
                std::set<int> extensionIds;
                for (const auto& extension : remote.headerExtensions)
                {
                    if (extension.id < 1 || extension.id > 14 || !extensionIds.insert(extension.id).second) continue;
                    const auto supported = std::find_if(capability->headerExtensions.begin(), capability->headerExtensions.end(),
                        [&](const auto& item) { return item.uri == extension.uri && item.attributes == extension.attributes; });
                    if (supported == capability->headerExtensions.end()) continue;
                    auto negotiated = extension;
                    negotiated.direction = AnswerDirection(extension.direction, supported->direction);
                    if (negotiated.direction != MediaDirection::Inactive) local.headerExtensions.push_back(std::move(negotiated));
                }
                if (CanSend(local.direction))
                {
                    local.ssrcs = capability->ssrcs;
                    local.ssrcGroups = capability->ssrcGroups;
                    local.msids = capability->msids;
                }
                ++accepted;
            }
        }
        BuildMediaSdp(local);
        result.sdp.medias.push_back(local.sdp);
        result.medias.push_back(std::move(local));
    }
    if (accepted == 0) return Reject("No compatible RTP codecs in the offer");
    // Ambiguous PTs need MID/SSRC demultiplexing, not implemented here.
    std::set<int> bundledPts;
    for (const auto& media : result.medias)
        for (const auto& codec : media.codecs)
            if (!bundledPts.insert(codec.payloadType).second)
                return Reject("Ambiguous bundled payload types require MID demultiplexing");
    for (const auto& mid : remote_offer_.bundle.mids)
        if (std::any_of(result.medias.begin(), result.medias.end(),
            [&](const auto& media) { return media.mid == mid && media.sdp.port != 0; }))
            result.bundle.mids.push_back(mid);
    if (!result.bundle.mids.empty())
    {
        std::string group = "BUNDLE";
        for (const auto& mid : result.bundle.mids) group += " " + mid;
        result.sdp.attributes.push_back({"group", std::move(group)});
    }
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
        [](const auto& media) { return media.sdp.port != 0; });
    const auto index = static_cast<size_t>(selected - local_answer_.medias.begin());
    const auto& remote = remote_offer_.medias[index];
    if (!dtls_->Configure(remote.dtls, selected->dtls.setup)) return Fail("DTLS configuration failed");
    ice_.SetRole(IceContext::Role::Controlled);
    ice_.SetLocalCredentials(local_answer_.ice.ufrag, local_answer_.ice.pwd);
    ice_.SetRemoteCredentials(remote.ice.ufrag, remote.ice.pwd);
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
    if (!dtls_started_) return true;
    if (!dtls_->Tick(nowMs)) return Fail("DTLS timeout or transport failure");
    return CompleteDtls();
}

void WebRtcSession::OnWebRtcDatagram(Protocol protocol, network::transport::ReceivedDatagram datagram)
try
{
    if ((state_ != WebRtcSessionState::Connecting && state_ != WebRtcSessionState::Connected) ||
        !datagram.IsValid() || Classifier::Classify(datagram.Data(), datagram.Size()) != protocol) return;
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
        if (media.sdp.port != 0 && (sending ? CanSend(media.direction) : CanReceive(media.direction)) &&
            std::any_of(media.codecs.begin(), media.codecs.end(),
                [pt](const auto& codec) { return codec.payloadType == pt; })) return true;
    return false;
}

void WebRtcSession::HandleEncryptedRtp(network::transport::ReceivedDatagram datagram)
{
    if (!srtp_ready_) return;
    auto plaintext = datagram.payload.ToVector();
    if (!srtp_->UnprotectRtp(plaintext) || !AllowsRtp(plaintext, false)) return;
    endpoint_->OnMediaPacket(ReceivedMediaPacket(MediaPacketType::Rtp, datagram.transport_id,
        datagram.receive_time_ms, std::move(plaintext)));
}

void WebRtcSession::HandleEncryptedRtcp(network::transport::ReceivedDatagram datagram)
{
    if (!srtp_ready_) return;
    auto plaintext = datagram.payload.ToVector();
    if (!srtp_->UnprotectRtcp(plaintext) ||
        !Classifier::IsRtcp(plaintext.data(), plaintext.size())) return;
    endpoint_->OnMediaPacket(ReceivedMediaPacket(MediaPacketType::Rtcp, datagram.transport_id,
        datagram.receive_time_ms, std::move(plaintext)));
}

bool WebRtcSession::SendRtp(std::vector<uint8_t> packet)
{
    return state_ == WebRtcSessionState::Connected && srtp_ready_ && AllowsRtp(packet, true) &&
        srtp_->ProtectRtp(packet) && transport_->Send(Protocol::Rtp, packet.data(), packet.size()) == SendResult::Ok;
}

bool WebRtcSession::SendRtcp(std::vector<uint8_t> packet)
{
    return state_ == WebRtcSessionState::Connected && srtp_ready_ &&
        Classifier::IsRtcp(packet.data(), packet.size()) && srtp_->ProtectRtcp(packet) &&
        transport_->Send(Protocol::Rtcp, packet.data(), packet.size()) == SendResult::Ok;
}
} // namespace protocol::webrtc
