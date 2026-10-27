#include "RoomSignalHandler.h"
#include "../WebRtcSessionContext.h"
#include "CryptoUtil.h"
#include "Room.h"
#include "Participant.h"
#include "TaskScheduler.h"
#include "EndpointBase.h"
#include "Sdp.h"
#include "SdpCodec.h"
#include "logger.h"
#include "SfuEndpoint.h"
#include "MediaTrack.h"
#include "MediaEndpointIngress.h"
#include "WebRtcMediaTransport.h"
#include "UdpServer.h"
#include "WebRtcCodec.h"
#include "WebRtcUdpMux.h"
#include <openssl/crypto.h>
#include <algorithm>
#include <atomic>
#include <set>
#include <stdexcept>
#include <utility>

namespace server::signaling
{
namespace
{
using namespace protocol::webrtc;
namespace auth = service::auth;
constexpr uint64_t kNegotiationTimeoutMs = 30000;
int AddressFamily(const std::string& ip)
{
    in_addr v4{};
    if (inet_pton(AF_INET, ip.c_str(), &v4) == 1)
        return v4.s_addr != htonl(INADDR_ANY) && v4.s_addr != htonl(INADDR_BROADCAST) && !IN_MULTICAST(ntohl(v4.s_addr)) ? AF_INET : AF_UNSPEC;
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
    video.headerExtensions.push_back({1, RtpHeaderExtensionUri::SDES_MID, MediaDirection::RecvOnly, {}});
    audio.headerExtensions = video.headerExtensions;
    return {std::move(video), std::move(audio)};
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
struct RoomSignalHandler::Membership
{
    // 已验签的身份和授权快照属于本次信令绑定；媒体包不携带 JWT。
    // 后续业务动作仍分别检查该快照对应的权限。
    auth::AuthContext caller;
    std::shared_ptr<room::Room> room;
    std::shared_ptr<room::Participant> participant;
    room::SignalingBinding binding;
    std::string resume_token;
    uint64_t reconnect_deadline_ms = 0;
    uint64_t notified_track_revision = 0;
};

struct RoomSignalHandler::RoomObservation
{
    std::weak_ptr<room::Room> room;
    std::shared_ptr<std::atomic<uint64_t>> revision = std::make_shared<std::atomic<uint64_t>>(0);
    media::EventBus::Subscription published;
    media::EventBus::Subscription unpublished;
};

struct RoomSignalHandler::Subscription
{
    std::string track_id;
    WebRtcMediaDescription media;
};

Json RoomSignalHandler::DispatchMessage(const std::string& connection, const SignalRequest& request, const ParticipantInit* init)
{
    using Type = SignalRequest::Type;
    if (request.type == Type::Unknown) return Error("Unsupported signaling message type");
    if (request.type == Type::Leave)
    {
        RemoveSession(connection);
        return Json{{"type", "closed"}};
    }
    if (request.type == Type::Join || request.type == Type::Resume)
        return init ? (init->reconnect ? ResumeRoom(connection, init->room_id, init->participant_id, init->resume_token, init->caller) : JoinRoom(connection, init->room_id, init->caller)) : Error("Invalid admission");

    const auto rejection = AuthorizeMessage(connection, request);
    if (rejection) return *rejection;
    switch (request.type)
    {
    case Type::Tracks: return ListTracks(connection);
    case Type::Offer:
        return Error("Room media renegotiation requires a new session; use restart_ice for ICE recovery");
    case Type::Answer:
    case Type::Rollback:
        return HandleNegotiationReply(*sessions_.at(connection), request);
    case Type::Publish:
    case Type::Subscribe:
    case Type::RestartIce:
        return HandleMediaOffer(connection, request);
    default: return Error("Unsupported signaling message type");
    }
}

std::optional<Json> RoomSignalHandler::AuthorizeMessage(const std::string& connection, const SignalRequest& request) const
{
    const auto member = memberships_.find(connection);
    if (member == memberships_.end()) return AuthError(auth::AuthError::Unauthenticated);
    using Type = SignalRequest::Type;
    auth::Action action;
    switch (request.type)
    {
    case Type::Tracks: action = auth::Action::ListTracks; break;
    case Type::Publish: action = auth::Action::Publish; break;
    case Type::Subscribe: action = auth::Action::Subscribe; break;
    case Type::Offer:
    case Type::Answer:
    case Type::Rollback:
    case Type::RestartIce:
    {
        const auto session = sessions_.find(connection);
        if (session == sessions_.end()) return Error("Publish or subscribe before negotiating a media session");
        action = session->second->subscriber ? auth::Action::Subscribe : auth::Action::Publish;
        break;
    }
    default: return Error("Unsupported signaling message type");
    }
    const auto decision = auth_->Authorize(member->second->caller, action, RoomResource(member->second->room->Id()));
    if (decision.Allowed()) return {};
    return AuthError(decision.error);
}


Json RoomSignalHandler::HandleNegotiationReply(Session& session, const SignalRequest& request)
{
    const auto& body = request.body;
    if (session.pending_offer_id.empty() || !body.contains("negotiation_id") ||
        !body["negotiation_id"].is_string() || body["negotiation_id"] != session.pending_offer_id)
        return Error("Answer or rollback does not match the pending offer");
    const bool isAnswer = request.type == SignalRequest::Type::Answer;
    if (isAnswer)
    {
        if (!body.contains("sdp") || !body["sdp"].is_string()) return Error("Expected an SDP answer");
        const auto& answer = body["sdp"].get_ref<const std::string&>();
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
    return Json{{"type", isAnswer ? "negotiated" : "rolled_back"},
        {"session_id", session.id}, {"negotiation_id", id}};
}

Json RoomSignalHandler::HandleMediaOffer(const std::string& connection, const SignalRequest& request)
{
    const auto& body = request.body;
    if (!body.contains("sdp") || !body["sdp"].is_string()) return Error("Expected an SDP offer");
    const auto& offer = body["sdp"].get_ref<const std::string&>();
    if (offer.empty() || offer.size() > kMaxSdpBytes) return Error("SDP must be between 1 and 65536 bytes");
    if (request.type == SignalRequest::Type::RestartIce)
    {
        auto& session = *sessions_.at(connection);
        WebRtcSessionDescription answer;
        if (!session.rtc->RestartIce(offer, answer)) return Error(session.rtc->LastError());
        return Json{{"type", "answer"}, {"session_id", session.id},
            {"sdp", sdp::Sdp::Serialize(answer)}};
    }
    if (sessions_.count(connection)) return Error("Leave and rejoin before changing the room media session");
    if (request.type == SignalRequest::Type::Publish) return CreateSession(connection, offer);
    return HandleSubscribe(connection, request, offer);
}

Json RoomSignalHandler::HandleSubscribe(const std::string& connection, const SignalRequest& request,
                                          const std::string& offer)
{
    const auto& body = request.body;
    if (!body.contains("tracks") || !body["tracks"].is_array() || body["tracks"].empty() || body["tracks"].size() > 8)
        return Error("Expected 1-8 track/MID bindings");
    std::vector<Subscription> subscriptions;
    std::set<std::string> trackIds, mids;
    for (const auto& binding : body["tracks"])
    {
        const auto trackId = binding.at("track_id").get<std::string>();
        const auto mid = binding.at("mid").get<std::string>();
        if (mid.empty() || mid.size() > 16 || !trackIds.insert(trackId).second || !mids.insert(mid).second)
            return Error("Duplicate or invalid track/MID binding");
        auto media = FindPublishedMedia(connection, trackId);
        if (!media) return Error("Track is unavailable in this room");
        Subscription subscription{trackId, std::move(*media)};
        subscription.media.mid = mid;
        subscription.media.direction = MediaDirection::SendOnly;
        subscription.media.ssrcs.clear();
        subscription.media.ssrcGroups.clear();
        subscription.media.msids = {"packetia " + trackId};
        for (auto& extension : subscription.media.headerExtensions) extension.direction = MediaDirection::SendOnly;
        uint32_t ssrc = 0;
        do
        {
            if (!utils::SecureRandomBytes(reinterpret_cast<uint8_t*>(&ssrc), sizeof(ssrc)))
                return Error("Could not generate sender SSRC");
        } while (!ssrc || std::any_of(subscriptions.begin(), subscriptions.end(),
            [&](const auto& previous) { return previous.media.ssrcs.front().ssrc == ssrc; }));
        subscription.media.ssrcs.push_back({ssrc, {{"cname", "packetia"}}});
        subscriptions.push_back(std::move(subscription));
    }
    return CreateSession(connection, offer, subscriptions);
}

std::optional<sdp::SdpMedia> RoomSignalHandler::FindPublishedMedia(const std::string& connection,
                                                            const std::string& trackId) const
{
    const auto& member = *memberships_.at(connection);
    for (const auto& item : memberships_)
    {
        if (item.first == connection || item.second->room != member.room) continue;
        const auto track = item.second->participant->GetPublishedTrack(trackId);
        const auto source = sessions_.find(item.first);
        if (!track || source == sessions_.end() || source->second->subscriber) continue;
        for (const auto& media : source->second->rtc->LocalDescription().medias)
            if (media.mid == track->info().mid && media.port && media.direction == MediaDirection::RecvOnly)
                return media;
    }
    return std::nullopt;
}

Json RoomSignalHandler::CreateSession(const std::string& connection, const std::string& offerSdp,
                                        const std::vector<Subscription>& subscriptions)
{
    WebRtcSessionDescription offer;
    std::string error;
    if (!sdp::Sdp::Parse(offerSdp, sdp::SdpProfile::WebRtc, SdpType::Offer, offer, error))
        return Error("Invalid SDP offer");
    WebRtcSessionOptions options;
    if (!utils::SecureRandomHex(8, options.ice.ufrag) || !utils::SecureRandomHex(24, options.ice.pwd))
        return Error("Could not generate ICE credentials");
    options.ice.candidates = {"1 1 UDP 2130706431 " + config_.webrtc.public_ip + " " + std::to_string(udp_->LocalAddress().Port()) + " typ host"};
    options.ice.endOfCandidates = true;
    if (sessions_.size() >= config_.webrtc.max_sessions) return Error("WebRTC session limit reached");
    options.medias = Capabilities();
    if (!subscriptions.empty())
    {
        options.medias.clear();
        for (const auto& subscription : subscriptions) options.medias.push_back(subscription.media);
    }
    options.singleCodecPerMedia = true;
    options.iceTimeoutMs = std::max<uint64_t>(30000, config_.webrtc.reconnect_timeout_ms + uint64_t{10000});
    auto dtls = CreateDtlsTransport();
    auto srtp = CreateSrtpTransport();
    if (!dtls || !srtp) return Error("Encryption backend is unavailable");

    auto entry = std::make_shared<Session>();
    // 与 RTSP 共用注册表及进程级 ID 分配器。
    entry->id = utils::EndpointBase::NextEndpointId();
    entry->ufrag = options.ice.ufrag;
    entry->room_managed = memberships_.count(connection) != 0;
    entry->subscriber = !subscriptions.empty();
    // 先建立资源归属，注册及分配失败时也能回滚 UDP 和媒体资源。
    auto& session = *sessions_.emplace(connection, std::move(entry)).first->second;
    const auto fail = [&](std::string detail)
    {
        RemoveMediaSession(connection);
        return Error(detail);
    };
    const auto datagram = mux_->Register(session.id, session.ufrag);
    if (!datagram) return fail("Could not register ICE session");
    const auto weakMux = std::weak_ptr<WebRtcUdpMux>(mux_);
    const auto weakSession = std::weak_ptr<Session>(sessions_.at(connection));
    options.onSelectedPeer = [weakMux, weakSession](const network::SocketAddr& peer)
    {
        const auto mux = weakMux.lock();
        const auto session = weakSession.lock();
        return mux && session && mux->BindPeer(session->ufrag, peer);
    };
    options.onIceRestart = [weakMux, weakSession](const std::string& previous, const std::string& next)
    {
        const auto mux = weakMux.lock();
        const auto session = weakSession.lock();
        std::string replacement = next;
        if (!mux || !session || !mux->Restart(previous, next)) return false;
        session->ufrag.swap(replacement);
        return true;
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
    options.onNegotiated = [weakEndpoint, subscriptions](const auto& local, const auto& remote)
    {
        const auto endpoint = weakEndpoint.lock();
        if (!endpoint) return false;
        if (!subscriptions.empty())
        {
            for (const auto& subscription : subscriptions)
            {
                const auto media = std::find_if(local.medias.begin(), local.medias.end(),
                    [&](const auto& item) { return item.mid == subscription.media.mid; });
                if (media == local.medias.end() || !media->port || media->direction != MediaDirection::SendOnly ||
                    media->codecs.size() != 1) return false;
                RtpCodecParameters source, downstream;
                const auto& codec = subscription.media.codecs.front();
                if (!NegotiateRtpCodec(codec, codec, source) ||
                    !NegotiateRtpCodec(media->codecs.front(), media->codecs.front(), downstream) ||
                    source.encodingName != downstream.encodingName || source.clockRate != downstream.clockRate ||
                    source.channels != downstream.channels || source.fmtp != downstream.fmtp) return false;
            }
            return true;
        }
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
    options.onNetworkPathChanged = [weak = std::weak_ptr<media::transport::WebRtcMediaTransport>(session.transport)]
        (const WebRtcNetworkPath& path)
    {
        if (const auto transport = weak.lock()) transport->OnNetworkPathChanged(path);
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
    const auto member = memberships_.find(connection);
    if (member != memberships_.end())
    {
        auto& membership = *member->second;
        session.room_binding = std::make_shared<room::ParticipantSession>(session.id,
            session.subscriber ? room::ParticipantSessionRole::Subscribe : room::ParticipantSessionRole::Publish,
            session.endpoint);
        if (!membership.participant->BindMediaSession(session.room_binding))
            return fail("Could not bind participant media session");
        if (subscriptions.empty())
        {
            for (const auto& media : answer.medias)
            {
                if (!media.port || media.direction != MediaDirection::RecvOnly) continue;
                media::TrackMetadata info;
                info.sid = membership.participant->Id() + ":" + media.mid;
                info.mid = media.mid;
                info.type = media.media == "video" ? media::MediaType::Video : media::MediaType::Audio;
                info.mimeType = media.media + "/" + media.codecs.front().encodingName;
                session.room_tracks.push_back(info.sid);
                if (!membership.room->PublishTrack(membership.participant->Id(), std::make_shared<media::MediaTrack>(info),
                    0, static_cast<uint8_t>(media.codecs.front().payloadType), media.mid)) return fail("Could not publish room track");
            }
        }
        else
        {
            for (const auto& subscription : subscriptions)
            {
                const auto media = std::find_if(answer.medias.begin(), answer.medias.end(),
                    [&](const auto& item) { return item.mid == subscription.media.mid; });
                rtsp::RtpSenderTrackConfig sender;
                sender.local_ssrc = media->ssrcs.front().ssrc;
                sender.payload_type = static_cast<uint8_t>(media->codecs.front().payloadType);
                sender.sample_rate = media->codecs.front().clockRate;
                for (const auto& extension : media->headerExtensions)
                    if (extension.uri == RtpHeaderExtensionUri::SDES_MID)
                    { sender.mid_extension_id = static_cast<uint8_t>(extension.id); sender.mid = media->mid; }
                const auto codec = media->media == "video" ? CodecId::H264 : CodecId::OPUS;
                session.room_subscriptions.push_back(subscription.track_id);
                if (!session.endpoint->ConfigureSubscription(subscription.track_id, sender, session.transport, codec) ||
                    !membership.room->SubscribeTrack(membership.participant->Id(), subscription.track_id))
                    return fail("Could not connect room subscription");
            }
        }
    }
    return Json{{"type", "answer"}, {"session_id", session.id}, {"sdp", sdp::Sdp::Serialize(answer)}};
}


void RoomSignalHandler::OnSignalingClosed(const std::string& connection)
{
    const auto found = memberships_.find(connection);
    if (!started_ || found == memberships_.end()) { RemoveSession(connection); return; }
    auto& member = *found->second;
    if (!member.participant->UnbindSignaling(member.binding)) return;
    member.reconnect_deadline_ms = Timestamp::NowMs() + config_.webrtc.reconnect_timeout_ms;
    member.participant->SetState(room::ParticipantState::Reconnecting);
    const auto session = sessions_.find(connection);
    if (session != sessions_.end())
    {
        session->second->rtc->RollbackNegotiation();
        session->second->pending_offer_id.clear();
        session->second->negotiation_deadline_ms = 0;
    }
}

Json RoomSignalHandler::ResumeRoom(const std::string& connection, const std::string& roomId,
    const std::string& participantId, const std::string& resumeToken, const auth::AuthContext& caller)
{
    if (memberships_.count(connection) || sessions_.count(connection)) return Error("Connection is already bound");
    const auto index = participant_connections_.find(participantId);
    if (index == participant_connections_.end()) return Error("Participant recovery expired");
    const auto found = memberships_.find(index->second);
    if (found == memberships_.end()) return Error("Participant recovery expired");
    auto& member = *found->second;
    // 新凭证须证明相同签发方、身份与权限；恢复连接不能隐式变更既有媒体权限。
    if (caller.issuer != member.caller.issuer || caller.subject_id != member.caller.subject_id ||
        caller.video.room != member.caller.video.room || caller.video.room_join != member.caller.video.room_join ||
        caller.video.can_publish != member.caller.video.can_publish || caller.video.can_subscribe != member.caller.video.can_subscribe)
        return AuthError(auth::AuthError::Forbidden);
    if (member.room->Id() != roomId || resumeToken.size() != member.resume_token.size() ||
        CRYPTO_memcmp(resumeToken.data(), member.resume_token.data(), resumeToken.size()) != 0)
        return Error("Invalid participant recovery credentials");
    if (!member.reconnect_deadline_ms) return Error("Participant is already connected");
    const auto previous = index->second;
    if (Timestamp::NowMs() >= member.reconnect_deadline_ms)
    {
        ++stats_.recovery_expired;
        RemoveSession(previous);
        return Error("Participant recovery expired");
    }
    const auto media = sessions_.find(previous);
    if (media != sessions_.end() && (media->second->rtc->State() == WebRtcSessionState::Failed ||
        media->second->rtc->State() == WebRtcSessionState::Closed))
    {
        ++stats_.media_closed;
        RemoveSession(previous);
        return Error("Media session expired; join again");
    }
    room::SignalingBinding nextBinding{connection, member.binding.generation + 1};
    auto bindingConnection = connection;
    auto indexedConnection = connection;
    const auto response = Json{{"type", "resumed"}, {"room_id", roomId}, {"participant_id", participantId},
        {"subject_id", member.caller.subject_id},
        {"resume_token", member.resume_token}, {"signaling_generation", nextBinding.generation},
        {"ice_restart", media != sessions_.end()}};
    // 先预留两个映射项再迁移归属，分配失败后原参与者仍可重试恢复。
    memberships_.reserve(memberships_.size() + 1);
    if (media != sessions_.end()) sessions_.emplace(connection, media->second);
    try { memberships_.emplace(connection, nullptr); }
    catch (...) { sessions_.erase(connection); throw; }
    if (!member.participant->BindSignaling(std::move(bindingConnection)))
    {
        memberships_.erase(connection);
        sessions_.erase(connection);
        return Error("Could not bind recovered participant");
    }
    member.binding = std::move(nextBinding);
    member.reconnect_deadline_ms = 0;
    // 旧连接已经入队的通知未必送达；新连接在 resumed 响应后重新获得当前快照。
    member.notified_track_revision = 0;
    member.participant->SetState(room::ParticipantState::Joined);
    memberships_.at(connection) = std::move(memberships_.at(previous));
    memberships_.erase(previous);
    sessions_.erase(previous);
    index->second.swap(indexedConnection);
    ++stats_.resume_succeeded;
    return response;
}


Json RoomSignalHandler::ListTracks(const std::string& connection) const
{
    const auto member = memberships_.find(connection);
    if (member == memberships_.end()) return Error("Join a room first");
    auto tracks = Json::array();
    for (const auto& participant : member->second->room->GetParticipants())
        for (const auto& track : participant->GetPublishedTracks())
        {
            const auto info = track->info();
            tracks.push_back({{"track_id", info.sid}, {"publisher_id", participant->Id()},
                {"kind", info.type == media::MediaType::Video ? "video" : "audio"}, {"mime_type", info.mimeType}});
        }
    std::sort(tracks.begin(), tracks.end(), [](const auto& a, const auto& b) { return a.at("track_id") < b.at("track_id"); });
    return Json{{"type", "tracks"}, {"tracks", tracks}};
}

void RoomSignalHandler::ObserveRoom(const std::shared_ptr<room::Room>& target)
{
    const auto found = room_observations_.find(target->Id());
    if (found != room_observations_.end() && found->second->room.lock() == target) return;
    auto observation = std::make_unique<RoomObservation>();
    observation->room = target;
    const std::weak_ptr<std::atomic<uint64_t>> weak = observation->revision;
    // Bus 回调只更新一个有界的版本槽，不触碰房间、连接或 handler 生命周期。
    const auto changed = [weak](const auto& event) {
        if (const auto revision = weak.lock())
        {
            auto previous = revision->load();
            while (previous < event.snapshot.revision &&
                   !revision->compare_exchange_weak(previous, event.snapshot.revision)) {}
        }
    };
    observation->published = target->Events().Subscribe<media::TrackPublishedEvent>(changed);
    observation->unpublished = target->Events().Subscribe<media::TrackUnpublishedEvent>(changed);
    room_observations_[target->Id()] = std::move(observation);
}

void RoomSignalHandler::NotifyTrackChanges()
{
    // 仅在 owner 的 Tick 中读业务状态；初始响应和请求响应已先进入响应通道。
    for (auto it = room_observations_.begin(); it != room_observations_.end();)
    {
        const auto room = rooms_.find(it->first);
        if (room == rooms_.end() || it->second->room.lock() != room->second)
            it = room_observations_.erase(it);
        else ++it;
    }
    std::unordered_map<std::string, Json> snapshots;
    for (auto& item : memberships_)
    {
        auto& member = *item.second;
        if (member.reconnect_deadline_ms || !emit_) continue;
        const auto observed = room_observations_.find(member.room->Id());
        if (observed == room_observations_.end()) continue;
        const auto revision = observed->second->revision->load();
        if (member.notified_track_revision == revision) continue;
        if (!auth_->Authorize(member.caller, auth::Action::ListTracks, RoomResource(member.room->Id())).Allowed())
            continue;
        auto snapshot = snapshots.find(member.room->Id());
        if (snapshot == snapshots.end())
        {
            auto message = ListTracks(item.first);
            message["type"] = "tracks_changed";
            message["room_id"] = member.room->Id();
            message["revision"] = revision;
            snapshot = snapshots.emplace(member.room->Id(), std::move(message)).first;
        }
        if (emit_(item.first, snapshot->second, false)) member.notified_track_revision = revision;
        // 队列拒绝时不推进版本；关闭由现有通道负责，仍可写的连接会在后续 Tick 重试最新快照。
    }
}

void RoomSignalHandler::LeaveRoom(const std::string& connection)
{
    const auto found = memberships_.find(connection);
    if (found == memberships_.end()) return;
    auto membership = std::move(found->second);
    memberships_.erase(found);
    participant_connections_.erase(membership->participant->Id());
    membership->room->Leave(membership->participant->Id());
    if (membership->room->ParticipantCount() == 0)
    {
        room_observations_.erase(membership->room->Id());
        rooms_.erase(membership->room->Id());
    }
}


void RoomSignalHandler::RemoveSession(const std::string& connection)
{
    LeaveRoom(connection);
    RemoveMediaSession(connection);
}

void RoomSignalHandler::RemoveMediaSession(const std::string& connection)
{
    const auto found = sessions_.find(connection);
    if (found == sessions_.end()) return;
    auto session = std::move(found->second);
    sessions_.erase(found);
    const auto member = memberships_.find(connection);
    if (member != memberships_.end() && session->room_binding)
    {
        for (const auto& track : session->room_tracks) member->second->room->UnpublishTrack(track);
        for (const auto& track : session->room_subscriptions)
            member->second->room->UnsubscribeTrack(member->second->participant->Id(), track);
        member->second->participant->UnbindMediaSession(session->room_binding);
    }
    if (session->rtc) session->rtc->stop();
    if (session->transport) session->transport->Close();
    if (mux_) mux_->Unregister(session->ufrag);
    if (session->endpoint_registered) utils::EndpointManager::Instance().Remove(session->id);
    if (session->endpoint) session->endpoint->Stop();
}

bool RoomSignalHandler::Renegotiate(uint64_t sessionId, const std::vector<sdp::SdpMedia>& medias, std::string& error)
{
    bool accepted = false;
    const auto perform = [&]
    {
        if (!started_) { error = "WebRTC service is stopped"; return; }
        const auto found = std::find_if(sessions_.begin(), sessions_.end(),
            [&](const auto& item) { return item.second->id == sessionId; });
        if (found == sessions_.end()) { error = "Unknown WebRTC session"; return; }
        auto& session = *found->second;
        if (session.room_managed) { error = "Room media renegotiation requires a new session"; return; }
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
                {"negotiation_id", offer.origin.sess_version}, {"sdp", sdp}};
            session.pending_offer_id = offer.origin.sess_version;
            if (!emit_ || !emit_(found->first, message, false)) throw std::runtime_error("Could not queue SDP offer");
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
    };
    perform();
    return accepted;
}


RoomSignalHandler::RoomSignalHandler(std::shared_ptr<TaskScheduler> owner, config::AppConfig config,
    std::shared_ptr<media::IEncodedFramePublisher> publisher,
    std::shared_ptr<const service::auth::AuthService> auth, WebRtcServiceStats& stats)
    : scheduler_(std::move(owner)), config_(std::move(config)), publisher_(std::move(publisher)),
      auth_(std::move(auth)), stats_(stats) {}
RoomSignalHandler::~RoomSignalHandler() { Stop(); }

bool RoomSignalHandler::Start()
{
    if (started_) return true;
    if (AddressFamily(config_.webrtc.public_ip) == AF_UNSPEC || !CreateDtlsTransport() || !CreateSrtpTransport()) return false;
    udp_ = std::make_shared<network::UdpServer>(scheduler_);
    if (!udp_->Start(config_.listen_ip, config_.udp_port, false)) return false;
    const auto local = udp_->LocalAddress();
    if (!udp_->IsWritable() || !local.Port() || local.ss.ss_family != AddressFamily(config_.webrtc.public_ip))
    { Stop(); return false; }
    mux_ = std::make_shared<WebRtcUdpMux>(udp_);
    udp_->SetHandler(mux_);
    started_ = true;
    return true;
}
void RoomSignalHandler::Stop()
{
    started_ = false;
    room_observations_.clear();
    while (!sessions_.empty()) RemoveSession(sessions_.begin()->first);
    while (!memberships_.empty()) LeaveRoom(memberships_.begin()->first);
    participant_connections_.clear(); rooms_.clear();
    if (mux_) mux_->Close();
    if (udp_) { udp_->SetHandler({}); udp_->Stop(); }
    mux_.reset(); udp_.reset(); emit_ = {};
}

Json RoomSignalHandler::Handle(const std::string& connection, const SignalRequest& request, const ParticipantInit* init)
{
    if (!started_) return Error("WebRTC service is stopped");
    const bool had_media = sessions_.count(connection) != 0;
    const bool had_member = HasMember(connection);
    try { return DispatchMessage(connection, request, init); }
    catch (...)
    {
        // 解析或分配失败只回收本次新建资源；不得误删原参与者及既有媒体。
        if (!had_media) RemoveMediaSession(connection);
        if (!had_member) LeaveRoom(connection);
        for (auto it = rooms_.begin(); it != rooms_.end();)
            if (!it->second || !it->second->ParticipantCount())
            { room_observations_.erase(it->first); it = rooms_.erase(it); }
            else ++it;
        return Error("Invalid signaling request");
    }
}

Json RoomSignalHandler::JoinRoom(const std::string& connection, const std::string& roomId, auth::AuthContext caller)
{
    if (sessions_.count(connection) || HasMember(connection)) return Error("Connection has already joined or negotiated");
    for (const auto& entry : memberships_)
        if (entry.second->room->Id() == roomId && entry.second->caller.subject_id == caller.subject_id)
            return Error("Identity is already in this room; resume the existing participant");
    if (memberships_.size() >= config_.webrtc.max_sessions) return Error("Room participant limit reached");
    std::string id, token;
    if (!utils::SecureRandomHex(8, id)) return Error("Could not generate participant ID");
    if (!utils::SecureRandomHex(32, token)) return Error("Could not generate resume token");
    id = "p-" + id;
    const auto response = Json{{"type", "joined"}, {"room_id", roomId}, {"participant_id", id},
        {"subject_id", caller.subject_id}, {"resume_token", token},
        {"reconnect_timeout_ms", config_.webrtc.reconnect_timeout_ms}};
    const auto existing = rooms_.find(roomId);
    std::shared_ptr<room::Room> target;
    if (existing != rooms_.end()) target = existing->second;
    else
    {
        room::RoomOptions options;
        options.auto_subscribe = false;
        options.max_participants = config_.webrtc.max_sessions;
        target = std::make_shared<room::Room>(room::RoomInfo{roomId, roomId}, options);
    }
    ObserveRoom(target);
    auto member = std::make_unique<Membership>();
    member->notified_track_revision = room_observations_.at(roomId)->revision->load();
    member->caller = std::move(caller); member->room = target;
    member->participant = std::make_shared<room::Participant>(room::ParticipantIdentity{
        id, member->caller.name.empty() ? member->caller.subject_id : member->caller.name, member->caller.subject_id});
    if (!member->participant->BindSignaling(connection)) return Error("Could not bind participant signaling");
    member->binding = member->participant->GetSignaling();
    member->resume_token = std::move(token);
    auto participant = member->participant;
    // 提前登记回滚所需的索引；后续失败由 Handle 的本次资源边界清理。
    memberships_.emplace(connection, std::move(member));
    participant_connections_.emplace(id, connection);
    rooms_.emplace(roomId, target);
    if (!target->Join(participant)) { RemoveSession(connection); return Error("Could not join room"); }
    ++stats_.join_succeeded;
    return response;
}

std::size_t RoomSignalHandler::ReconnectingCount() const
{
    std::size_t count = 0;
    for (const auto& item : memberships_) if (item.second->reconnect_deadline_ms) ++count;
    return count;
}
void RoomSignalHandler::Tick()
{
    if (!started_) return;
    const auto now = Timestamp::NowMs();
    std::set<std::string> expired;
    for (const auto& item : memberships_)
        if (item.second->reconnect_deadline_ms && now >= item.second->reconnect_deadline_ms)
        { ++stats_.recovery_expired; expired.insert(item.first); }
    for (const auto& item : sessions_)
    {
        auto& session = *item.second;
        if (!session.rtc->Tick(now))
        {
            if (!expired.count(item.first)) ++stats_.media_closed;
            expired.insert(item.first); continue;
        }
        if (session.endpoint && session.rtc->State() == WebRtcSessionState::Connected)
            session.endpoint->ScheduleRecoveryTick();
        if (session.negotiation_deadline_ms && now >= session.negotiation_deadline_ms)
        {
            session.rtc->RollbackNegotiation();
            const auto id = std::move(session.pending_offer_id);
            session.pending_offer_id.clear(); session.negotiation_deadline_ms = 0;
            ++stats_.negotiation_timeouts;
            if (!emit_ || !emit_(item.first, Json{{"type", "error"},
                {"error", "Negotiation timed out; previous media retained"}, {"negotiation_id", id}}, false))
                expired.insert(item.first);
        }
    }
    for (const auto& connection : expired)
    {
        const auto member = memberships_.find(connection);
        const bool suspended = member != memberships_.end() && member->second->reconnect_deadline_ms != 0;
        RemoveSession(connection);
        if (!suspended && emit_) emit_(connection, Error("WebRTC connection closed or timed out"), true);
    }
    NotifyTrackChanges();
}

}
