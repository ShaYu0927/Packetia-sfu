#include "WebRtcService.h"

#include "EventLoop.h"
#include "EndpointBase.h"
#include "Sdp.h"
#include "SdpCodec.h"
#include "logger.h"
#include "media/endpoint/MediaEndpoint.h"
#include "media/transport/MediaEndpointIngress.h"
#include "media/transport/WebRtcMediaTransport.h"
#include "network/UdpServer.h"
#include "network/websocket/WsServer.h"
#include "protocol/WebRtc/WebRtcCodec.h"
#include "protocol/WebRtc/WebRtcUdpMux.h"
#include "third/nlohmann/json.hpp"

#include <openssl/crypto.h>
#include <openssl/rand.h>

#include <algorithm>
#include <set>
#include <stdexcept>
#include <utility>

namespace server
{
namespace
{
using Json = nlohmann::json;
using namespace protocol::webrtc;
constexpr size_t kMaxSdpBytes = 64 * 1024;
// Account for JSON escaping of an otherwise valid SDP.
constexpr size_t kMaxMessageBytes = 6 * kMaxSdpBytes + 4096;
constexpr uint64_t kNegotiationTimeoutMs = 30000;

std::string Error(const std::string& error)
{
    return Json{{"type", "error"}, {"error", error}}.dump();
}

bool RandomCredential(size_t bytes, std::string& result)
{
    std::vector<unsigned char> random(bytes);
    if (RAND_bytes(random.data(), static_cast<int>(random.size())) != 1) return false;
    static constexpr char hex[] = "0123456789abcdef";
    result.clear();
    result.reserve(bytes * 2);
    for (const auto byte : random)
    {
        result.push_back(hex[byte >> 4]);
        result.push_back(hex[byte & 15]);
    }
    return true;
}

int AddressFamily(const std::string& ip)
{
    in_addr v4{};
    if (inet_pton(AF_INET, ip.c_str(), &v4) == 1)
        return v4.s_addr != htonl(INADDR_ANY) && v4.s_addr != htonl(INADDR_BROADCAST) &&
            !IN_MULTICAST(ntohl(v4.s_addr)) ? AF_INET : AF_UNSPEC;
    in6_addr v6{};
    if (inet_pton(AF_INET6, ip.c_str(), &v6) == 1)
        return !IN6_IS_ADDR_UNSPECIFIED(&v6) && !IN6_IS_ADDR_MULTICAST(&v6) ? AF_INET6 : AF_UNSPEC;
    return AF_UNSPEC;
}

std::vector<WebRtcMediaDescription> Capabilities()
{
    WebRtcMediaDescription video, audio;
    video.media = "video";
    audio.media = "audio";
    video.direction = audio.direction = MediaDirection::RecvOnly;
    video.rtcpMux = audio.rtcpMux = true;
    sdp::Sdp::SetCodecs(video, {sdp::SdpCodec::H264(96)});
    sdp::Sdp::SetCodecs(audio, {sdp::SdpCodec::Opus(111)});
    // Receive MID for bundled track lookup; no transport feedback is promised.
    video.headerExtensions.push_back({1, RtpHeaderExtensionUri::SDES_MID, MediaDirection::RecvOnly, {}});
    audio.headerExtensions = video.headerExtensions;
    return {std::move(video), std::move(audio)};
}

void SelectPrimaryCodecs(WebRtcSessionDescription& offer, const std::vector<WebRtcMediaDescription>& capabilities)
{
    for (auto& media : offer.medias)
    {
        auto capability = std::find_if(capabilities.begin(), capabilities.end(),
            [&](const auto& item) { return !item.mid.empty() && item.mid == media.mid; });
        if (capability == capabilities.end())
            capability = std::find_if(capabilities.begin(), capabilities.end(),
                [&](const auto& item) { return item.mid.empty() && item.media == media.media; });
        if (capability == capabilities.end()) continue;
        for (const auto& format : media.fmts)
        {
            const auto codec = std::find_if(media.codecs.begin(), media.codecs.end(),
                [&](const auto& item) { return std::to_string(item.payloadType) == format; });
            if (codec == media.codecs.end()) continue;
            if (!std::any_of(capability->codecs.begin(), capability->codecs.end(), [&](const auto& supported)
            {
                RtpCodecParameters negotiated;
                return NegotiateRtpCodec(*codec, supported, negotiated);
            })) continue;
            const auto selected = *codec;
            media.fmts = {std::to_string(selected.payloadType)};
            media.codecs = {selected};
            break;
        }
    }
}

void CopyPrimarySources(const WebRtcMediaDescription& remote, WebRtcMediaDescription& track)
{
    std::set<uint32_t> repair;
    for (const auto& group : remote.ssrcGroups)
        if (group.semantics == "FID" || group.semantics == "FEC" || group.semantics == "FEC-FR")
            for (size_t i = 1; i < group.ssrcs.size(); ++i) repair.insert(group.ssrcs[i]);
    for (const auto& source : remote.ssrcs)
        if (source.ssrc && !repair.count(source.ssrc)) track.ssrcs.push_back(source);
}

class PublishedEndpoint final : public media::SfuEndpoint
{
public:
    PublishedEndpoint(uint64_t id, std::shared_ptr<media::IEncodedFramePublisher> publisher)
        : media::SfuEndpoint(id, std::move(publisher))
    {
        session_id_ = "webrtc-" + std::to_string(id);
        stream_id_ = "webrtc";
    }
};
}

struct WebRtcService::Session
{
    uint64_t id = 0;
    bool endpoint_registered = false;
    std::string ufrag;
    std::shared_ptr<WebRtcSession> rtc;
    std::shared_ptr<media::transport::WebRtcMediaTransport> transport;
    std::shared_ptr<media::transport::MediaEndpointIngress> ingress;
    std::shared_ptr<media::SfuEndpoint> endpoint;
    std::string pending_offer_id;
    uint64_t negotiation_deadline_ms = 0;
};

WebRtcService::WebRtcService(EventLoop* loop, config::AppConfig config,
                             std::shared_ptr<media::IEncodedFramePublisher> publisher)
    : loop_(loop), config_(std::move(config)), publisher_(std::move(publisher))
{
}

WebRtcService::~WebRtcService()
{
    Stop();
}

bool WebRtcService::Start()
{
    if (started_) return scheduler_ && !scheduler_->IsStopped();
    if (!loop_ || weak_from_this().expired())
    {
        LOG_ERROR("WebRtcService Start: requires an event loop and shared service ownership");
        return false;
    }
    if (!config_.webrtc.enabled || config_.webrtc.token.empty() || config_.webrtc.max_sessions == 0)
    {
        LOG_ERROR("WebRtcService Start: enable WebRTC with a nonempty token and positive session limit");
        return false;
    }
    if (AddressFamily(config_.webrtc.public_ip) == AF_UNSPEC)
    {
        LOG_ERROR("WebRtcService Start: public_ip must be a numeric, non-wildcard unicast address");
        return false;
    }
    scheduler_ = loop_->GetTaskScheduler();
    if (!scheduler_ || scheduler_->IsStopped())
    {
        LOG_ERROR("WebRtcService Start: event loop is not running");
        return false;
    }
    bool result = false;
    try
    {
        scheduler_->Invoke([&]
        {
            // Fail startup when the configured build cannot serve encrypted media.
            if (!CreateDtlsTransport() || !CreateSrtpTransport())
            {
                LOG_ERROR("WebRtcService Start: DTLS/SRTP backend unavailable; check OpenSSL and libsrtp2 support");
                return;
            }
            udp_ = std::make_shared<network::UdpServer>(scheduler_);
            if (!udp_->Start(config_.listen_ip, config_.udp_port, false))
            {
                LOG_ERROR("WebRtcService Start: failed to bind UDP listener");
                return;
            }
            const auto local = udp_->LocalAddress();
            if (!udp_->IsWritable() || local.Port() == 0 ||
                local.ss.ss_family != AddressFamily(config_.webrtc.public_ip))
            {
                LOG_ERROR("WebRtcService Start: public_ip family must match the active UDP listener");
                return;
            }

            mux_ = std::make_shared<WebRtcUdpMux>(udp_);
            udp_->SetHandler(mux_);
            network::websocket::WsServerOptions options;
            options.limits.max_message_bytes = kMaxMessageBytes;
            ws_ = std::make_shared<network::websocket::WsServer>(scheduler_, options);
            const auto weak = weak_from_this();
            ws_->SetOnMessage([weak](const std::string& connection, const std::string& message)
            {
                const auto self = weak.lock();
                return self ? self->OnMessage(connection, message) : std::string{};
            });
            ws_->SetOnClose([weak](const std::string& connection)
            {
                if (auto self = weak.lock()) self->RemoveSession(connection);
            });
            if (!ws_->Start(config_.listen_ip, config_.websocket_port))
            {
                LOG_ERROR("WebRtcService Start: failed to bind WebSocket listener");
                return;
            }
            started_ = true;
            timer_ = scheduler_->AddTimer([weak]
            {
                const auto self = weak.lock();
                return self && self->Tick();
            }, 100);
            result = timer_ != 0;
            if (!result) LOG_ERROR("WebRtcService Start: failed to create session timer");
        });
    }
    catch (...)
    {
        Stop();
        throw;
    }
    if (!result) Stop();
    return result;
}

void WebRtcService::Stop()
{
    if (scheduler_) scheduler_->Invoke([this] { StopOnOwner(); });
    else StopOnOwner();
    scheduler_.reset();
}

void WebRtcService::StopOnOwner()
{
    started_ = false;
    if (timer_ && scheduler_) scheduler_->RemoveTimer(timer_);
    timer_ = 0;
    if (ws_)
    {
        ws_->SetOnMessage({});
        ws_->SetOnClose({});
    }
    while (!sessions_.empty()) RemoveSession(sessions_.begin()->first);
    if (mux_) mux_->Close();
    if (udp_) udp_->SetHandler({});
    if (ws_) ws_->Stop();
    if (udp_) udp_->Stop();
    mux_.reset();
    ws_.reset();
    udp_.reset();
}

std::string WebRtcService::OnMessage(const std::string& connection, const std::string& message)
{
    if (!started_) return Error("WebRTC service is stopped");
    if (message.size() > kMaxMessageBytes) return Error("Signaling message is too large");
    const bool hadSession = sessions_.count(connection) != 0;
    try
    {
        // Limit nesting before materializing untrusted JSON on the I/O thread.
        const auto request = Json::parse(message, [](int depth, Json::parse_event_t, Json&)
        {
            if (depth > 8) throw std::invalid_argument("JSON nesting limit exceeded");
            return true;
        });
        if (!request.is_object() || !request.contains("type") || !request["type"].is_string())
            return Error("Expected a signaling message type");
        const auto type = request["type"].get<std::string>();
        if (type == "close")
        {
            RemoveSession(connection);
            return Json{{"type", "closed"}}.dump();
        }
        if (type != "offer" && type != "answer" && type != "rollback")
            return Error("Unsupported signaling message type");
        if (!request.contains("token") || !request["token"].is_string()) return Error("Unauthorized");
        const auto token = request["token"].get<std::string>();
        if (token.size() != config_.webrtc.token.size() ||
            CRYPTO_memcmp(token.data(), config_.webrtc.token.data(), token.size()) != 0)
            return Error("Unauthorized");
        const auto found = sessions_.find(connection);
        if (type != "offer")
        {
            if (found == sessions_.end()) return Error("No session to negotiate");
            auto& session = *found->second;
            if (session.pending_offer_id.empty() || !request.contains("negotiation_id") ||
                !request["negotiation_id"].is_string() || request["negotiation_id"] != session.pending_offer_id)
                return Error("Answer or rollback does not match the pending offer");
            if (type == "answer")
            {
                if (!request.contains("sdp") || !request["sdp"].is_string()) return Error("Expected an SDP answer");
                const auto answer = request["sdp"].get<std::string>();
                if (answer.empty() || answer.size() > kMaxSdpBytes) return Error("Invalid SDP answer size");
                if (!session.rtc->ApplyRemoteAnswer(answer))
                {
                    const auto error = session.rtc->LastError();
                    if (session.rtc->SignalingState() == sdp::SdpNegotiationState::Stable)
                    {
                        session.pending_offer_id.clear();
                        session.negotiation_deadline_ms = 0;
                    }
                    return Error(error);
                }
            }
            else session.rtc->RollbackNegotiation();
            const auto id = std::move(session.pending_offer_id);
            session.pending_offer_id.clear();
            session.negotiation_deadline_ms = 0;
            return Json{{"type", type == "answer" ? "negotiated" : "rolled_back"},
                {"session_id", session.id}, {"negotiation_id", id}}.dump();
        }
        if (!request.contains("sdp") || !request["sdp"].is_string()) return Error("Expected an SDP offer");
        const auto offer = request["sdp"].get<std::string>();
        if (offer.empty() || offer.size() > kMaxSdpBytes) return Error("SDP must be between 1 and 65536 bytes");
        if (found != sessions_.end()) return UpdateSession(*found->second, offer);
        if (sessions_.size() >= config_.webrtc.max_sessions) return Error("WebRTC session limit reached");
        return CreateSession(connection, offer);
    }
    catch (const std::exception&)
    {
        // Do not echo parser diagnostics: they can contain credentials.
        if (!hadSession) RemoveSession(connection);
        return Error("Invalid signaling request");
    }
}

std::string WebRtcService::CreateSession(const std::string& connection, const std::string& offerSdp)
{
    WebRtcSessionDescription offer;
    std::string error;
    if (!sdp::Sdp::Parse(offerSdp, sdp::SdpProfile::WebRtc, SdpType::Offer, offer, error))
        return Error("Invalid SDP offer");
    WebRtcSessionOptions options;
    if (!RandomCredential(8, options.ice.ufrag) || !RandomCredential(24, options.ice.pwd))
        return Error("Could not generate ICE credentials");
    options.ice.candidates = {"1 1 UDP 2130706431 " + config_.webrtc.public_ip + " " + std::to_string(udp_->LocalAddress().Port()) + " typ host"};
    options.ice.endOfCandidates = true;
    options.medias = Capabilities();
    SelectPrimaryCodecs(offer, options.medias);
    auto dtls = CreateDtlsTransport();
    auto srtp = CreateSrtpTransport();
    if (!dtls || !srtp) return Error("Encryption backend is unavailable");

    auto entry = std::make_unique<Session>();
    // RTSP shares this registry and uses EndpointBase's process-wide allocator.
    entry->id = utils::EndpointBase::NextEndpointId();
    entry->ufrag = options.ice.ufrag;
    // Establish ownership before registering resources so allocation failures
    // also roll back the UDP entry through OnMessage's RemoveSession.
    auto& session = *sessions_.emplace(connection, std::move(entry)).first->second;
    const auto fail = [&](std::string detail)
    {
        RemoveSession(connection);
        return Error(detail);
    };
    const auto datagram = mux_->Register(session.id, session.ufrag);
    if (!datagram) return fail("Could not register ICE session");
    const auto weakMux = std::weak_ptr<WebRtcUdpMux>(mux_);
    options.onSelectedPeer = [weakMux, ufrag = session.ufrag](const network::SocketAddr& peer)
    {
        const auto mux = weakMux.lock();
        return mux && mux->BindPeer(ufrag, peer);
    };
    const auto weakScheduler = std::weak_ptr<TaskScheduler>(scheduler_);
    session.transport = std::make_shared<media::transport::WebRtcMediaTransport>(session.id,
        [weakScheduler](std::function<void()> task)
        {
            const auto scheduler = weakScheduler.lock();
            return scheduler && scheduler->Post(std::move(task));
        });
    session.endpoint = std::make_shared<PublishedEndpoint>(session.id, publisher_);
    const auto weakEndpoint = std::weak_ptr<media::SfuEndpoint>(session.endpoint);
    options.onNegotiated = [weakEndpoint](const auto& local, const auto& remote)
    {
        const auto endpoint = weakEndpoint.lock();
        if (!endpoint) return false;
        std::vector<sdp::SdpMedia> tracks;
        for (const auto& negotiated : local.medias)
        {
            if (!negotiated.port || negotiated.direction == MediaDirection::Inactive) continue;
            if (negotiated.direction != MediaDirection::RecvOnly || negotiated.codecs.size() != 1) return false;
            auto track = negotiated;
            track.ssrcs.clear();
            track.ssrcGroups.clear();
            const auto source = std::find_if(remote.medias.begin(), remote.medias.end(),
                [&](const auto& media) { return media.mid == track.mid; });
            if (source == remote.medias.end()) return false;
            CopyPrimarySources(*source, track);
            tracks.push_back(std::move(track));
        }
        if (endpoint->GetState() == utils::EndpointBase::State::kInit && tracks.empty()) return false;
        return endpoint->ReplacePublishedTracks(tracks);
    };
    session.rtc = std::make_shared<WebRtcSession>(
        std::make_shared<WebRtcTransport>(session.id, datagram),
        std::move(dtls), std::move(srtp), session.transport, std::move(options));
    WebRtcSessionDescription answer;
    if (!session.rtc->ApplyRemoteOffer(offer) || !session.rtc->CreateLocalAnswer(answer))
        return fail(session.rtc->LastError());
    session.ingress = std::make_shared<media::transport::MediaEndpointIngress>(session.id);
    session.transport->SetPacketSink(session.ingress);
    if (!session.transport->BindSession(session.rtc)) return fail("Could not bind media transport");
    const auto weakTransport = std::weak_ptr<media::transport::WebRtcMediaTransport>(session.transport);
    session.endpoint->SetRtcpSendCallback([weakTransport](const uint8_t* data, size_t size)
    {
        const auto transport = weakTransport.lock();
        return transport && transport->Send(MediaPacketType::Rtcp, data, size) == SendResult::Ok;
    });
    if (!session.endpoint->Start()) return fail("Could not start WebRTC media endpoint");
    session.endpoint_registered = utils::EndpointManager::Instance().Add(session.endpoint);
    if (!session.endpoint_registered || !session.rtc->start())
        return fail("Could not start WebRTC media session");
    return Json{{"type", "answer"}, {"session_id", session.id}, {"sdp", sdp::Sdp::Serialize(answer)}}.dump();
}

std::string WebRtcService::UpdateSession(Session& session, const std::string& offerSdp)
{
    WebRtcSessionDescription offer, answer;
    std::string error;
    if (!sdp::Sdp::Parse(offerSdp, sdp::SdpProfile::WebRtc, SdpType::Offer, offer, error))
        return Error("Invalid SDP offer");
    auto capabilities = session.rtc->LocalDescription().medias;
    const auto defaults = Capabilities();
    capabilities.insert(capabilities.end(), defaults.begin(), defaults.end());
    SelectPrimaryCodecs(offer, capabilities);
    if (!session.rtc->ApplyRemoteOffer(offer)) return Error(session.rtc->LastError());
    if (!session.rtc->CreateLocalAnswer(answer))
    {
        const auto failure = session.rtc->LastError();
        session.rtc->RollbackNegotiation();
        return Error(failure);
    }
    return Json{{"type", "answer"}, {"session_id", session.id}, {"sdp", sdp::Sdp::Serialize(answer)}}.dump();
}

bool WebRtcService::Renegotiate(uint64_t sessionId, const std::vector<sdp::SdpMedia>& medias, std::string& error)
{
    if (!scheduler_) { error = "WebRTC service is stopped"; return false; }
    bool accepted = false;
    scheduler_->Invoke([&]
    {
        if (!started_) { error = "WebRTC service is stopped"; return; }
        const auto found = std::find_if(sessions_.begin(), sessions_.end(),
            [&](const auto& item) { return item.second->id == sessionId; });
        if (found == sessions_.end()) { error = "Unknown WebRTC session"; return; }
        auto& session = *found->second;
        const auto supported = Capabilities();
        for (const auto& media : medias)
        {
            if (!media.port) continue;
            if ((media.direction != MediaDirection::RecvOnly && media.direction != MediaDirection::Inactive) ||
                media.codecs.size() != 1 || !media.rtcpFeedback.empty() || !media.codecs.front().rtcpFeedback.empty())
            { error = "WebRTC publication requires one receive codec per track and no transport feedback"; return; }
            const auto kind = std::find_if(supported.begin(), supported.end(),
                [&](const auto& item) { return item.media == media.media; });
            if (kind == supported.end()) { error = "Unsupported published media kind"; return; }
            auto codec = media.codecs.front();
            std::transform(codec.encodingName.begin(), codec.encodingName.end(), codec.encodingName.begin(),
                [](unsigned char c) { return c >= 'A' && c <= 'Z' ? static_cast<char>(c + 'a' - 'A') : static_cast<char>(c); });
            RtpCodecParameters checked;
            if ((media.media == "video" ? codec.encodingName != "h264" : codec.encodingName != "opus") ||
                !NegotiateRtpCodec(codec, codec, checked))
            { error = "Unsupported published RTP codec parameters"; return; }
            for (const auto& extension : media.headerExtensions)
                if (extension.uri != RtpHeaderExtensionUri::SDES_MID || extension.id < 1 || extension.id > 14 ||
                    (extension.direction != MediaDirection::RecvOnly && extension.direction != MediaDirection::Inactive))
                { error = "Publication supports only the receive MID extension"; return; }
        }
        WebRtcSessionDescription offer;
        if (!session.rtc->CreateLocalOffer(medias, offer)) { error = session.rtc->LastError(); return; }
        try
        {
            const auto sdp = sdp::Sdp::Serialize(offer);
            if (sdp.size() > kMaxSdpBytes) throw std::length_error("SDP offer too large");
            const auto message = Json{{"type", "offer"}, {"session_id", session.id},
                {"negotiation_id", offer.origin.sess_version}, {"sdp", sdp}}.dump();
            session.pending_offer_id = offer.origin.sess_version;
            if (!ws_->SendText(found->first, message)) throw std::runtime_error("Could not queue SDP offer");
        }
        catch (const std::exception&)
        {
            session.rtc->RollbackNegotiation();
            session.pending_offer_id.clear();
            session.negotiation_deadline_ms = 0;
            error = "Could not queue the SDP offer";
            return;
        }
        session.negotiation_deadline_ms = Timestamp::NowMs() + kNegotiationTimeoutMs;
        error.clear();
        accepted = true;
    });
    return accepted;
}

void WebRtcService::RemoveSession(const std::string& connection)
{
    const auto found = sessions_.find(connection);
    if (found == sessions_.end()) return;
    auto session = std::move(found->second);
    sessions_.erase(found);
    if (session->rtc) session->rtc->stop();
    if (session->transport) session->transport->Close();
    if (mux_) mux_->Unregister(session->ufrag);
    if (session->endpoint_registered) utils::EndpointManager::Instance().Remove(session->id);
    if (session->endpoint) session->endpoint->Stop();
}

bool WebRtcService::Tick()
{
    if (!started_) return false;
    std::vector<std::string> expired;
    for (const auto& item : sessions_)
    {
        auto& session = *item.second;
        const auto now = Timestamp::NowMs();
        if (!session.rtc->Tick(now)) { expired.push_back(item.first); continue; }
        if (session.negotiation_deadline_ms && now >= session.negotiation_deadline_ms)
        {
            session.rtc->RollbackNegotiation();
            const auto id = std::move(session.pending_offer_id);
            session.pending_offer_id.clear();
            session.negotiation_deadline_ms = 0;
            if (!ws_->SendText(item.first, Json{{"type", "error"}, {"error", "Negotiation timed out; previous media retained"},
                {"negotiation_id", id}}.dump())) expired.push_back(item.first);
        }
    }
    for (const auto& connection : expired)
    {
        RemoveSession(connection);
        if (!ws_->SendText(connection, Error("WebRTC connection closed or timed out")))
            ws_->CloseConnection(connection);
    }
    return true;
}
}
