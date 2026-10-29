#include "RoomSignalHandler.h"
#include "../WebRtcSessionContext.h"
#include "CryptoUtil.h"
#include "Room.h"
#include "Participant.h"
#include "ParticipantMessages.h"
#include "TaskScheduler.h"
#include "EndpointBase.h"
#include "Sdp.h"
#include "SdpCodec.h"
#include "logger.h"
#include "SfuEndpoint.h"
#include "TrackPublication.h"
#include "MediaStreamRegistry.h"
#include "MediaStreamEvent.h"
#include "MediaTrackEvent.h"
#include "SdpTrackBinding.h"
#include "RtpTrackBinding.h"
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
#include <ctime>

namespace server::signaling
{
namespace
{
using namespace protocol::webrtc;
namespace auth = service::auth;
constexpr uint64_t kNegotiationTimeoutMs = 30000;
constexpr utils::ChannelLimits kDatagramQueueLimits{4 * 1024 * 1024, 4096};
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
    PublishedEndpoint(uint64_t id, std::shared_ptr<media::IEncodedFramePublisher> publisher,
                      const media::MediaStream::Ptr& stream)
        : media::SfuEndpoint(id, std::move(publisher))
    {
        session_id_ = "webrtc-" + std::to_string(id);
        stream_id_ = "webrtc";
        media_stream_id_ = stream ? stream->GetId() : 0;
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
    uint64_t notified_participant_revision = 0;
    bool capacity_reserved = false;
};

struct RoomSignalHandler::RoomObservation
{
    std::weak_ptr<room::Room> room;
    std::shared_ptr<std::atomic<uint64_t>> revision = std::make_shared<std::atomic<uint64_t>>(0);
    media::EventBus::Subscription published;
    media::EventBus::Subscription unpublished;
    std::shared_ptr<std::atomic<uint64_t>> participant_revision = std::make_shared<std::atomic<uint64_t>>(0);
    media::EventBus::Subscription participant_changed;
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
        return init ? (init->reconnect ? ResumeRoom(connection, init->room_id, init->participant_id, init->resume_token, init->caller) : JoinRoom(connection, *init)) : Error("Invalid admission");

    const auto rejection = AuthorizeMessage(connection, request);
    if (rejection) return *rejection;
    switch (request.type)
    {
    case Type::Tracks: return ListTracks(connection);
    case Type::Participants:
    case Type::Participant:
    case Type::UpdateParticipant:
    case Type::RemoveParticipant:
        return HandleParticipant(connection, request);
    case Type::Unpublish:
    case Type::Unsubscribe:
        return HandleMediaStop(connection, request);
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
    case Type::Participants:
    case Type::Participant: action = auth::Action::ListParticipants; break;
    case Type::RemoveParticipant: action = auth::Action::ManageParticipants; break;
    case Type::UpdateParticipant:
    {
        const auto& body = request.body;
        const bool self = !body.contains("participant_id") || body["participant_id"] == member->second->participant->Id();
        action = self && !body.contains("permissions") ? auth::Action::UpdateOwnParticipant : auth::Action::ManageParticipants;
        break;
    }
    case Type::Publish:
    case Type::Unpublish: action = auth::Action::Publish; break;
    case Type::Subscribe:
    case Type::Unsubscribe: action = auth::Action::Subscribe; break;
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
    if (sessions_.count(connection)) return Error("Stop the current media session with unpublish or unsubscribe before starting another");
    if (request.type == SignalRequest::Type::Publish) return CreateSession(connection, offer);
    return HandleSubscribe(connection, request, offer);
}

Json RoomSignalHandler::HandleMediaStop(const std::string& connection, const SignalRequest& request)
{
    const auto id = request.body.find("session_id");
    if (id == request.body.end() || !id->is_number_integer() ||
        (!id->is_number_unsigned() && id->get<int64_t>() <= 0) ||
        (id->is_number_unsigned() && id->get<uint64_t>() == 0))
        return Error("Expected a positive integer session_id from the media answer");

    const auto sessionId = id->get<uint64_t>();
    const bool subscriber = request.type == SignalRequest::Type::Unsubscribe;
    const auto session = sessions_.find(connection);
    const bool changed = session != sessions_.end();
    if (changed)
    {
        if (session->second->id != sessionId) return Error("Media session_id does not match the current session");
        if (session->second->subscriber != subscriber) return Error("Media stop action does not match the current session role");
        RemoveMediaSession(connection);
    }
    return Json{{"type", subscriber ? "unsubscribed" : "unpublished"},
        {"session_id", sessionId}, {"changed", changed}};
}

Json RoomSignalHandler::HandleSubscribe(const std::string& connection, const SignalRequest& request, const std::string& offer)
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

std::optional<sdp::SdpMedia> RoomSignalHandler::FindPublishedMedia(const std::string& connection, const std::string& trackId) const
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

Json RoomSignalHandler::CreateSession(const std::string& connection, const std::string& offerSdp, const std::vector<Subscription>& subscriptions)
{
    // 创建媒体会话：subscriptions 为空时发布，否则订阅。
    WebRtcSessionDescription offer;
    WebRtcSessionOptions options;
    std::string error;
    options.ice.candidates = {"1 1 UDP 2130706431 " + config_.webrtc.public_ip + " " + std::to_string(udp_port_) + " typ host"};
    options.ice.endOfCandidates = true;
    options.singleCodecPerMedia = true;
    options.iceTimeoutMs = std::max<uint64_t>(30000, config_.webrtc.reconnect_timeout_ms + uint64_t{10000});

    if (!sdp::Sdp::Parse(offerSdp, sdp::SdpProfile::WebRtc, SdpType::Offer, offer, error))
        return Error("Invalid SDP offer");

    if (!utils::SecureRandomHex(8, options.ice.ufrag) || !utils::SecureRandomHex(24, options.ice.pwd))
        return Error("Could not generate ICE credentials");

    // 发布使用接收能力；订阅使用源轨道生成的下行参数。
    options.medias = Capabilities();
    if (!subscriptions.empty())
    {
        options.medias.clear();
        for (const auto& subscription : subscriptions) options.medias.push_back(subscription.media);
    }

    auto dtls = CreateDtlsTransport();
    auto srtp = CreateSrtpTransport();
    if (!dtls || !srtp) return Error("Encryption backend is unavailable");

    auto entry = std::make_shared<Session>();

    entry->id = utils::EndpointBase::NextEndpointId();
    entry->ufrag = options.ice.ufrag;
    entry->room_managed = memberships_.count(connection) != 0;
    entry->subscriber = !subscriptions.empty();

    // 先登记归属，后续失败统一回收本次资源。
    auto& session = *sessions_.emplace(connection, std::move(entry)).first->second;
    const auto fail = [&](std::string detail)
    {
        RemoveMediaSession(connection);
        return Error(detail);
    };
    if (!capacity_->AcquireMedia()) return fail("WebRTC session limit reached");
    session.capacity_reserved = true;
    // 按 ICE ufrag 分流 UDP，并维护选路和重启后的映射。
    WebRtcUdpMux::Dispatch dispatch;
    if (datagrams_)
    {
        const std::weak_ptr<DatagramTasks> inbox = datagrams_;
        dispatch = [inbox](std::function<void()> task, size_t bytes) {
            const auto channel = inbox.lock();
            return channel && channel->Send(std::move(task), bytes) == utils::ChannelResult::Accepted;
        };
    }
    const auto datagram = mux_->Register(session.id, session.ufrag, std::move(dispatch));
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
    // 传输任务投递回 owner，异步回调通过弱引用访问资源。
    const auto weakScheduler = std::weak_ptr<TaskScheduler>(scheduler_);
    session.transport = std::make_shared<media::transport::WebRtcMediaTransport>(session.id,
        [weakScheduler](std::function<void()> task)
        {
            const auto scheduler = weakScheduler.lock();
            return scheduler && scheduler->Post(std::move(task));
        });
    const auto member = memberships_.find(connection);
    if (!session.subscriber)
    {
        // 房间负责发布归属，内核负责媒体资源；两者以本次发布实例关联。
        media::MediaStream::Source source{"webrtc", {}, {}};
        auto name = "webrtc-" + std::to_string(session.id);
        if (member != memberships_.end())
        {
            source.scope = member->second->room->Id();
            source.publisher_id = member->second->participant->Id();
            name = source.publisher_id;
        }
        session.media_stream = std::make_shared<media::MediaStream>(std::move(name), std::move(source));
    }
    session.endpoint = std::make_shared<PublishedEndpoint>(session.id, publisher_, session.media_stream);
    const auto weakEndpoint = std::weak_ptr<media::SfuEndpoint>(session.endpoint);
    options.onNegotiated = [weakEndpoint, subscriptions](const auto& local, const auto& remote)
    {
        const auto endpoint = weakEndpoint.lock();
        if (!endpoint) return false;
        if (!subscriptions.empty())
        {
            // 订阅只转发，协商结果必须兼容源编码。
            for (const auto& subscription : subscriptions)
            {
                const auto media = std::find_if(local.medias.begin(), local.medias.end(),
                    [&](const auto& item) { return item.mid == subscription.media.mid; });
                if (media == local.medias.end() || !media->port || media->direction != MediaDirection::SendOnly || media->codecs.size() != 1) return false;
                RtpCodecParameters source, downstream;
                const auto& codec = subscription.media.codecs.front();
                if (!NegotiateRtpCodec(codec, codec, source) ||
                    !NegotiateRtpCodec(media->codecs.front(), media->codecs.front(), downstream) ||
                    source.encodingName != downstream.encodingName || source.clockRate != downstream.clockRate ||
                    source.channels != downstream.channels || source.fmtp != downstream.fmtp) return false;
            }
            return true;
        }
        // 发布按 MID 匹配远端主 SSRC，更新端点轨道。
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
    // 完成 Offer/Answer 协商，再接通媒体入口和 RTCP 回传。
    WebRtcSessionDescription answer;
    if (!session.rtc->ApplyRemoteOffer(offer) || !session.rtc->CreateLocalAnswer(answer))
        return fail(session.rtc->LastError());
    if (session.media_stream)
    {
        std::vector<media::MediaTrackDescription> tracks;
        for (const auto& media : answer.medias)
        {
            if (!media.port || media.direction != MediaDirection::RecvOnly) continue;
            ::TrackInfo binding;
            media::MediaTrackDescription track;
            if (!session.endpoint->GetPublishedTrack(media.mid, binding))
                return fail("Could not find published endpoint track");
            // 使用 Endpoint 分配的稳定索引，不把 SDP 列表位置当作轨道身份。
            const auto context = media::RtpStreamContextBuilder::BuildTrackFromSdp(media, binding.track_index);
            std::size_t selected = 0;
            if (!media::SelectRtpPayload(context, selected, &error) ||
                !media::DescribeMediaTrack(context, selected, track, &error)) return fail(error);
            tracks.push_back(std::move(track));
        }
        if (!session.media_stream->ReplaceTracks(std::move(tracks), &error)) return fail(error);
        for (const auto& track : session.media_stream->GetTracks())
            if (!session.media_stream->BindTrackEndpoint(track.id, session.id))
                return fail("Could not bind core media track");
    }
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
    if (session.media_stream && (!session.media_stream->StartPublishing() ||
        !media::MediaStreamRegistry::Instance().Register(session.media_stream)))
        return fail("Could not register WebRTC publication");
    // 将媒体会话绑定到房间参与者。
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
            // 发布轨道登记到房间，供列表查询与订阅。
            for (const auto& media : answer.medias)
            {
                if (!media.port || media.direction != MediaDirection::RecvOnly) continue;
                media::TrackPublicationInfo info;
                info.sid = membership.participant->Id() + ":" + media.mid;
                info.mid = media.mid;
                info.type = media.media == "video" ? media::MediaType::Video : media::MediaType::Audio;
                info.mimeType = media.media + "/" + media.codecs.front().encodingName;
                ::TrackInfo binding;
                if (!session.endpoint->GetPublishedTrack(media.mid, binding))
                    return fail("Could not resolve core room track");
                media::MediaTrackDescription coreTrack;
                if (!session.media_stream->GetTrack(static_cast<media::TrackId>(binding.track_index), &coreTrack))
                    return fail("Could not resolve core track reference");
                info.media_track = coreTrack.ref;
                session.room_tracks.push_back(info.sid);
                if (!membership.room->PublishTrack(membership.participant->Id(), std::make_shared<media::TrackPublication>(info),
                    0, static_cast<uint8_t>(media.codecs.front().payloadType), media.mid)) return fail("Could not publish room track");
            }
        }
        else
        {
            // 使用订阅端协商的 SSRC、PT 和 MID 建立下行路由。
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
    member.room->NotifyParticipantChanged(member.participant->Id());
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
    // 恢复只证明相同身份、房间和路由；服务端的会内有效权限不会被新 JWT 覆盖。
    if (caller.issuer != member.caller.issuer || caller.subject_id != member.caller.subject_id ||
        caller.video.room != member.caller.video.room || !caller.video.room_join ||
        caller.assignment_id != member.caller.assignment_id)
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
    auto response = Json{{"type", "resumed"}, {"room_id", roomId}, {"participant_id", participantId},
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
    member.room->NotifyParticipantChanged(member.participant->Id());
    member.notified_participant_revision = member.room->ParticipantRevision();
    response["participant"] = ParticipantMessage(member.participant->GetInfo());
    response["revision"] = member.notified_participant_revision;
    memberships_.at(connection) = std::move(memberships_.at(previous));
    memberships_.erase(previous);
    sessions_.erase(previous);
    index->second.swap(indexedConnection);
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
    observation->published = target->Events().Subscribe<room::TrackPublishedEvent>(changed);
    observation->unpublished = target->Events().Subscribe<room::TrackUnpublishedEvent>(changed);
    observation->participant_revision->store(target->ParticipantRevision());
    const std::weak_ptr<std::atomic<uint64_t>> members = observation->participant_revision;
    observation->participant_changed = target->Events().Subscribe<room::ParticipantChangedEvent>([members](const auto& event) {
        if (const auto revision = members.lock())
        {
            auto previous = revision->load();
            while (previous < event.revision && !revision->compare_exchange_weak(previous, event.revision)) {}
        }
    });
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

Json RoomSignalHandler::ListParticipants(const std::shared_ptr<room::Room>& targetRoom, const SignalRequest& request) const
{
    const auto& body = request.body;
    const auto revision = targetRoom->ParticipantRevision();
    const auto mismatch = [&] {
        auto error = Error("Participant snapshot changed; restart pagination");
        error["code"] = "snapshot_changed";
        error["revision"] = revision;
        return error;
    };
    if (body.contains("revision"))
    {
        if (!body["revision"].is_number_integer() ||
            (body["revision"].is_number_integer() && !body["revision"].is_number_unsigned() && body["revision"].get<int64_t>() < 0))
            return Error("Invalid participant revision");
        if (body["revision"].get<uint64_t>() != revision) return mismatch();
    }
    if (request.type == SignalRequest::Type::Participant)
    {
        if (!body.contains("participant_id") || !body["participant_id"].is_string()) return Error("Expected participant_id");
        const auto participant = targetRoom->GetParticipant(body["participant_id"].get<std::string>());
        if (!participant) return {{"type", "error"}, {"code", "participant_not_found"}, {"error", "Participant not found in this room"}};
        return {{"type", "participant"}, {"room_id", targetRoom->Id()}, {"revision", revision},
            {"participant", ParticipantMessage(participant->GetInfo())}};
    }
    std::size_t limit = 20;
    if (body.contains("limit"))
    {
        if (!body["limit"].is_number_integer() || body["limit"] < 1 || body["limit"] > 100)
            return Error("limit must be an integer from 1 to 100");
        limit = body["limit"].get<std::size_t>();
    }
    std::string cursor;
    if (body.contains("cursor"))
    {
        if (!body["cursor"].is_string() || body["cursor"].get_ref<const std::string&>().size() > 64)
            return Error("Invalid participant cursor");
        cursor = body["cursor"].get<std::string>();
        if (!cursor.empty() && !body.contains("revision")) return Error("A continuation cursor requires revision");
    }
    auto participants = targetRoom->GetParticipants();
    std::sort(participants.begin(), participants.end(), [](const auto& a, const auto& b) { return a->Id() < b->Id(); });
    auto page = Json::array();
    std::size_t bytes = 0;
    std::string next;
    for (const auto& participant : participants)
    {
        if (participant->Id() <= cursor) continue;
        auto value = ParticipantMessage(participant->GetInfo());
        const auto size = value.dump().size();
        // 同时限制人数和编码后的字节数，包含 JSON 转义的实际开销。
        if (page.size() == limit || (!page.empty() && bytes + size > kMaxMessageBytes / 2))
        { next = page.back().at("participant_id").get<std::string>(); break; }
        bytes += size;
        page.push_back(std::move(value));
    }
    return {{"type", "participants"}, {"room_id", targetRoom->Id()}, {"revision", revision},
        {"participants", std::move(page)}, {"total", participants.size()}, {"next_cursor", next}};
}

Json RoomSignalHandler::HandleParticipant(const std::string& connection, const SignalRequest& request)
{
    return ApplyParticipantCommand(memberships_.at(connection)->room, connection, request);
}

Json RoomSignalHandler::ApplyParticipantCommand(std::shared_ptr<room::Room> targetRoom,
    const std::string& connection, const SignalRequest& request)
{
    const auto& body = request.body;
    using Type = SignalRequest::Type;
    const bool list = request.type == Type::Participants;
    const bool query = request.type == Type::Participant;
    const bool update = request.type == Type::UpdateParticipant;
    for (auto it = body.begin(); it != body.end(); ++it)
    {
        const auto& key = it.key();
        if (key == "type" || (key == "participant_id" && !list) ||
            (key == "revision" && (list || query)) || (list && (key == "limit" || key == "cursor")) ||
            (update && (key == "name" || key == "metadata" || key == "attributes" || key == "permissions"))) continue;
        return Error("Unknown participant request field: " + key);
    }
    if (list || query) return ListParticipants(targetRoom, request);
    std::string id = connection.empty() ? std::string{} : memberships_.at(connection)->participant->Id();
    if (body.contains("participant_id"))
    {
        if (!body["participant_id"].is_string()) return Error("Expected participant_id");
        id = body["participant_id"].get<std::string>();
    }
    else if (!update) return Error("Expected participant_id");
    const auto target = targetRoom->GetParticipant(id);
    if (!target) return {{"type", "error"}, {"code", "participant_not_found"}, {"error", "Participant not found in this room"}};
    const auto index = participant_connections_.find(id);
    if (index == participant_connections_.end()) return Error("Participant binding not found");
    const auto targetConnection = index->second;
    auto& member = *memberships_.at(targetConnection);
    if (!update)
    {
        if (targetConnection == connection) return Error("Use leave to remove yourself");
        const bool connected = member.reconnect_deadline_ms == 0;
        const auto roomId = targetRoom->Id();
        RemoveSession(targetConnection);
        // 删除成员和恢复凭据之后才结束连接，迟到的关闭回调不会重新创建成员。
        if (connected && emit_) emit_(targetConnection, Json{{"type", "removed"}, {"reason", "kicked"},
            {"room_id", roomId}, {"participant_id", id}}, true);
        return {{"type", "participant_removed"}, {"room_id", roomId}, {"participant_id", id},
            {"revision", targetRoom->ParticipantRevision()}};
    }
    if (!body.contains("name") && !body.contains("metadata") && !body.contains("attributes") && !body.contains("permissions"))
        return Error("Expected a participant update");
    auto info = target->GetInfo();
    Json error;
    if (!ApplyParticipantPatch(body, info, error)) return error;
    const auto media = sessions_.find(targetConnection);
    const bool stopMedia = media != sessions_.end() &&
        (media->second->subscriber ? !info.permissions.can_subscribe : !info.permissions.can_publish);
    if (!targetRoom->UpdateParticipant(id, std::move(info.identity.name), std::move(info.properties), info.permissions))
        return Error("Participant left during update");
    auto& grant = member.caller.video;
    grant.can_publish = info.permissions.can_publish;
    grant.can_subscribe = info.permissions.can_subscribe;
    grant.room_admin = info.permissions.room_admin;
    grant.can_update_own_metadata = info.permissions.can_update_own_metadata;
    if (stopMedia) RemoveMediaSession(targetConnection);
    const auto snapshot = ParticipantMessage(target->GetInfo());
    const auto revision = targetRoom->ParticipantRevision();
    if (body.contains("permissions") && targetConnection != connection && !member.reconnect_deadline_ms && emit_)
        emit_(targetConnection, Json{{"type", "permissions_changed"}, {"room_id", targetRoom->Id()},
            {"revision", revision}, {"participant", snapshot}, {"media_stopped", stopMedia}}, false);
    return {{"type", "participant_updated"}, {"room_id", targetRoom->Id()}, {"revision", revision},
        {"participant", snapshot}, {"media_stopped", stopMedia}};
}

void RoomSignalHandler::RetireAssignment(const std::string& roomId, const std::string& id)
{
    const auto key = id; // 调用者可能引用即将删除的 active 项。
    const auto active = assignments_.find(roomId);
    // 还要覆盖控制面最近签发、尚未用于 join 的 60 秒凭证。
    const auto expiry = std::max(static_cast<int64_t>(std::time(nullptr)) + 120,
        active == assignments_.end() ? int64_t{0} : active->second.expires);
    retired_assignments_.try_emplace(key, RetiredAssignment{roomId, expiry, false});
    if (active != assignments_.end() && active->second.id == key) assignments_.erase(active);
}
std::vector<RoomRelease> RoomSignalHandler::ReleasedRooms() const
{
    std::vector<RoomRelease> result;
    for (const auto& item : retired_assignments_)
        if (!item.second.reported)
        {
            result.push_back({item.second.room, item.first});
            if (result.size() == 16) break;
        }
    return result;
}
Json RoomSignalHandler::ControlRoom(const std::string& roomId, const std::string& assignment, const Json& command)
{
    const auto reject = [](const char* code, const char* reason) {
        return Json{{"type", "error"}, {"code", code}, {"error", reason}};
    };
    if (!started_) return reject("unavailable", "SFU is stopped");
    if (assignment.empty()) return reject("stale_assignment", "Missing room assignment");
    const auto type = command.value("type", "");
    const auto active = assignments_.find(roomId);
    const auto retired = retired_assignments_.find(assignment);
    if (type == "release_ack")
    {
        if (retired != retired_assignments_.end() && retired->second.room == roomId) retired->second.reported = true;
        return {{"type", "release_acknowledged"}};
    }
    if (active != assignments_.end() && active->second.id != assignment)
        return reject("stale_assignment", "Room belongs to another assignment");
    const auto room = rooms_.find(roomId);
    if (room != rooms_.end() && active == assignments_.end())
        return reject("stale_assignment", "Room has no matching assignment");
    if (type == "reclaim_room" || type == "end_room")
    {
        if (type == "reclaim_room" && room != rooms_.end() && room->second->ParticipantCount())
            return {{"type", "room_busy"}, {"released", false}};
        if (retired_assignments_.size() >= 65536 && retired == retired_assignments_.end())
            return reject("unavailable", "Retired assignment capacity reached");
        // owner 上先封住旧代次，再清理成员；随后抵达的旧 JWT 也不能重新创建房间。
        RetireAssignment(roomId, assignment);
        std::vector<std::string> connections;
        for (const auto& item : memberships_) if (item.second->room->Id() == roomId) connections.push_back(item.first);
        for (const auto& connection : connections)
        {
            const bool connected = !memberships_.at(connection)->reconnect_deadline_ms;
            RemoveSession(connection);
            if (connected && emit_) emit_(connection, {{"type", "removed"}, {"reason", "room_closed"}, {"room_id", roomId}}, true);
        }
        return {{"type", type == "end_room" ? "room_ended" : "room_reclaimed"}, {"room_id", roomId}, {"released", true}};
    }
    if (retired != retired_assignments_.end()) return reject("stale_assignment", "Room assignment was retired");
    SignalRequest request; request.type = SignalRequest::ParseType(type); request.body = command;
    if (request.type != SignalRequest::Type::Participants && request.type != SignalRequest::Type::Participant &&
        request.type != SignalRequest::Type::UpdateParticipant && request.type != SignalRequest::Type::RemoveParticipant)
        return reject("invalid_request", "Unsupported room management command");
    if (room == rooms_.end())
    {
        if (request.type == SignalRequest::Type::Participants)
            return {{"type", "participants"}, {"room_id", roomId}, {"revision", 0}, {"total", 0},
                {"participants", Json::array()}, {"next_cursor", ""}};
        return reject("participant_not_found", "Participant not found in this room");
    }
    return ApplyParticipantCommand(room->second, {}, request);
}

void RoomSignalHandler::NotifyParticipantChanges()
{
    for (auto& item : memberships_)
    {
        auto& member = *item.second;
        if (member.reconnect_deadline_ms || !emit_) continue;
        const auto observed = room_observations_.find(member.room->Id());
        if (observed == room_observations_.end()) continue;
        const auto revision = observed->second->participant_revision->load();
        if (revision == member.notified_participant_revision) continue;
        // 通知只使客户端快照失效；不缓存无界事件队列，也不广播全部成员资料。
        if (emit_(item.first, Json{{"type", "participants_changed"}, {"room_id", member.room->Id()},
            {"revision", revision}}, false)) member.notified_participant_revision = revision;
    }
}

void RoomSignalHandler::LeaveRoom(const std::string& connection)
{
    const auto found = memberships_.find(connection);
    if (found == memberships_.end()) return;
    auto membership = std::move(found->second);
    memberships_.erase(found);
    if (membership->capacity_reserved) capacity_->ReleaseParticipant();
    participant_connections_.erase(membership->participant->Id());
    membership->room->Leave(membership->participant->Id());
    if (membership->room->ParticipantCount() == 0)
    {
        const auto active = assignments_.find(membership->room->Id());
        if (active != assignments_.end()) RetireAssignment(membership->room->Id(), active->second.id);
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
    if (session->capacity_reserved) capacity_->ReleaseMedia();
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
    // 停止传输和 Endpoint 后再注销，旧发布实例的回调不会移除同名新流。
    if (session->media_stream) media::MediaStreamRegistry::Instance().Remove(session->media_stream);
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
    std::shared_ptr<const service::auth::AuthService> auth, WebRtcServiceStats& stats,
    std::shared_ptr<RoomCapacity> capacity)
    : scheduler_(std::move(owner)), config_(std::move(config)), publisher_(std::move(publisher)),
      auth_(std::move(auth)), stats_(stats),
      capacity_(capacity ? std::move(capacity) : std::make_shared<RoomCapacity>(config_.webrtc.max_sessions)) {}
RoomSignalHandler::~RoomSignalHandler() { Stop(); }

void RoomSignalHandler::AttachNetwork(std::shared_ptr<network::UdpServer> udp,
    std::shared_ptr<WebRtcUdpMux> mux, uint16_t port, int address_family, std::shared_ptr<utils::ChannelBudget> budget)
{
    udp_ = std::move(udp); mux_ = std::move(mux); udp_port_ = port;
    network_family_ = address_family;
    owns_network_ = false;
    const std::weak_ptr<TaskScheduler> owner = scheduler_;
    auto postToOwner = [owner](std::function<void()> task) {
        const auto executor = owner.lock();
        if (!executor) return false;
        return executor->Post(std::move(task));
    };
    auto deliverDatagram = [](std::function<void()> task) { task(); };
    datagrams_ = std::make_shared<DatagramTasks>(
        std::move(postToOwner), std::move(deliverDatagram), std::move(budget), kDatagramQueueLimits);
}

bool RoomSignalHandler::Start()
{
    if (started_) return true;
    if (AddressFamily(config_.webrtc.public_ip) == AF_UNSPEC || !CreateDtlsTransport() || !CreateSrtpTransport()) return false;
    if (!owns_network_)
    {
        if (!udp_ || !mux_ || !udp_port_ || !udp_->IsWritable()) return false;
        if (network_family_ != AddressFamily(config_.webrtc.public_ip)) return false;
        ObserveMediaRegistry();
        return started_ = true;
    }
    udp_ = std::make_shared<network::UdpServer>(scheduler_);
    if (!udp_->Start(config_.listen_ip, config_.udp_port, false)) return false;
    const auto local = udp_->LocalAddress();
    udp_port_ = local.Port();
    if (!udp_->IsWritable() || !local.Port() || local.ss.ss_family != AddressFamily(config_.webrtc.public_ip))
    { Stop(); return false; }
    mux_ = std::make_shared<WebRtcUdpMux>(udp_);
    udp_->SetHandler(mux_);
    ObserveMediaRegistry();
    started_ = true;
    return true;
}
void RoomSignalHandler::Stop()
{
    started_ = false;
    core_track_changed_.Reset();
    core_stream_removed_.Reset();
    if (datagrams_) datagrams_->Cancel();
    room_observations_.clear();
    while (!sessions_.empty()) RemoveSession(sessions_.begin()->first);
    while (!memberships_.empty()) LeaveRoom(memberships_.begin()->first);
    participant_connections_.clear(); rooms_.clear(); assignments_.clear(); retired_assignments_.clear();
    if (owns_network_)
    {
        if (mux_) mux_->Close();
        if (udp_) { udp_->SetHandler({}); udp_->Stop(); }
    }
    mux_.reset(); udp_.reset(); datagrams_.reset(); emit_ = {};
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

Json RoomSignalHandler::JoinRoom(const std::string& connection, const ParticipantInit& init)
{
    const auto& roomId = init.room_id;
    const auto& caller = init.caller;
    if (!caller.assignment_id.empty())
    {
        const auto active = assignments_.find(roomId);
        if (retired_assignments_.count(caller.assignment_id) ||
            (active != assignments_.end() && active->second.id != caller.assignment_id))
            return Json{{"type", "error"}, {"code", "stale_assignment"}, {"error", "Room assignment expired; obtain a new admission"}};
        if (retired_assignments_.size() >= 65536) return Error("Room recovery capacity reached");
    }
    if (sessions_.count(connection) || HasMember(connection)) return Error("Connection has already joined or negotiated");
    for (const auto& entry : memberships_)
        if (entry.second->room->Id() == roomId && entry.second->caller.subject_id == caller.subject_id)
            return Error("Identity is already in this room; resume the existing participant");
    std::string id, token;
    if (!utils::SecureRandomHex(8, id)) return Error("Could not generate participant ID");
    if (!utils::SecureRandomHex(32, token)) return Error("Could not generate resume token");
    id = "p-" + id;
    auto response = Json{{"type", "joined"}, {"room_id", roomId}, {"participant_id", id},
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
    member->caller = caller; member->room = target;
    member->participant = std::make_shared<room::Participant>(room::ParticipantIdentity{
        id, init.participant.identity.name, member->caller.subject_id}, init.participant.properties,
        ParticipantPermissions(caller.video));
    if (!member->participant->BindSignaling(connection)) return Error("Could not bind participant signaling");
    member->binding = member->participant->GetSignaling();
    member->resume_token = std::move(token);
    auto participant = member->participant;
    // 提前登记回滚所需的索引；后续失败由 Handle 的本次资源边界清理。
    memberships_.emplace(connection, std::move(member));
    if (!capacity_->AcquireParticipant()) { RemoveSession(connection); return Error("Room participant limit reached"); }
    memberships_.at(connection)->capacity_reserved = true;
    participant_connections_.emplace(id, connection);
    rooms_.emplace(roomId, target);
    if (!caller.assignment_id.empty())
    {
        auto& assignment = assignments_[roomId];
        assignment.id = caller.assignment_id;
        assignment.expires = std::max(assignment.expires, caller.expires_at_unix_seconds + 30);
    }
    if (!target->Join(participant)) { RemoveSession(connection); return Error("Could not join room"); }
    memberships_.at(connection)->notified_participant_revision = target->ParticipantRevision();
    response["revision"] = target->ParticipantRevision();
    response["participant"] = ParticipantMessage(participant->GetInfo());
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
    ReconcileMediaTracks();
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
    NotifyParticipantChanges();
    for (auto it = retired_assignments_.begin(); it != retired_assignments_.end();)
        if (it->second.reported && it->second.expires <= static_cast<int64_t>(std::time(nullptr))) it = retired_assignments_.erase(it);
        else ++it;
}

void RoomSignalHandler::ObserveMediaRegistry()
{
    const std::weak_ptr<std::atomic<bool>> dirty = core_tracks_dirty_;
    const auto changed = [dirty](const auto&) {
        if (const auto flag = dirty.lock()) flag->store(true);
    };
    auto& events = media::MediaStreamRegistry::Instance().Events();
    core_track_changed_ = events.Subscribe<media::MediaTrackStateEvent>(changed);
    core_stream_removed_ = events.Subscribe<media::StreamUnregisteredEvent>(changed);
}

void RoomSignalHandler::ReconcileMediaTracks()
{
    // 全局事件只更新一个有界脏标志；房间、接收器及协议资源均回到 owner 清理。
    if (!core_tracks_dirty_->exchange(false)) return;
    auto& registry = media::MediaStreamRegistry::Instance();
    std::vector<std::string> empty;
    for (const auto& item : sessions_)
    {
        auto& session = *item.second;
        if (!session.media_stream) continue;
        if (!registry.FindById(session.media_stream->GetId()))
        { empty.push_back(item.first); continue; }
        const auto member = memberships_.find(item.first);
        if (member == memberships_.end()) continue;
        for (auto it = session.room_tracks.begin(); it != session.room_tracks.end();)
        {
            const auto track = member->second->participant->GetPublishedTrack(*it);
            if (track && !registry.FindTrack(track->info().media_track))
            {
                member->second->room->UnpublishTrack(*it);
                it = session.room_tracks.erase(it);
            }
            else ++it;
        }
        if (session.room_tracks.empty() && session.room_managed) empty.push_back(item.first);
    }
    for (const auto& connection : empty) RemoveMediaSession(connection);
}

}
