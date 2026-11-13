#pragma once
#include "AppConfig.h"
#include "AdmissionValidator.h"
#include "WebRtcServiceStats.h"
#include "SdpMode.h"
#include "RoomOwner.h"
#include "BoundedChannel.h"
#include "EventBus.h"
#include <memory>
#include <optional>
#include <unordered_map>

class TaskScheduler;
namespace network { class UdpServer; }
namespace media { class IEncodedFramePublisher; }
namespace protocol::webrtc { class WebRtcUdpMux; }
namespace room { class Room; }
namespace server { struct WebRtcSessionContext; }

namespace server::signaling
{
// 所有方法由房间 owner 串行调用。只向响应通道投递，不持有或调用 WebSocket。
class RoomSignalHandler
{
public:
    using Emit = std::function<bool(const std::string&, Json, bool close)>;
    RoomSignalHandler(std::shared_ptr<TaskScheduler> owner, config::AppConfig config,
                      std::shared_ptr<media::IEncodedFramePublisher> publisher,
                      std::shared_ptr<const service::auth::AuthService> auth, WebRtcServiceStats& stats,
                      std::shared_ptr<RoomCapacity> capacity = {});
    ~RoomSignalHandler();
    bool Start();
    void AttachNetwork(std::shared_ptr<network::UdpServer> udp,
                       std::shared_ptr<protocol::webrtc::WebRtcUdpMux> mux, uint16_t port, int address_family,
                       std::shared_ptr<utils::ChannelBudget> budget);
    void Stop();
    void SetEmitter(Emit emit) { emit_ = std::move(emit); }
    Json Handle(const std::string& connection, const SignalRequest& request, const ParticipantInit* init = nullptr);
    void OnSignalingClosed(const std::string& connection);
    void Tick();
    Json ControlRoom(const std::string& room, const std::string& assignment, const Json& command);
    std::vector<RoomRelease> ReleasedRooms() const;
    bool Renegotiate(uint64_t session, const std::vector<sdp::SdpMedia>& medias, std::string& error);
    std::size_t ParticipantCount() const { return memberships_.size(); }
    std::size_t ReconnectingCount() const;
    bool HasMember(const std::string& connection) const { return memberships_.count(connection) != 0; }
    void RejectNewAdmission(const std::string& connection) { RemoveSession(connection); }
private:
    using Session = server::WebRtcSessionContext;
    struct MediaSessions
    {
        std::shared_ptr<Session> publish;
        std::shared_ptr<Session> subscribe;
        std::shared_ptr<Session>& Role(bool subscriber) { return subscriber ? subscribe : publish; }
        const std::shared_ptr<Session>& Role(bool subscriber) const { return subscriber ? subscribe : publish; }
        bool Empty() const { return !publish && !subscribe; }
    };
    struct Membership;
    struct Subscription;
    struct RoomObservation;
    using DatagramTasks = utils::BoundedChannel<std::function<void()>>;
    Json DispatchMessage(const std::string&, const SignalRequest&, const ParticipantInit*);
    std::optional<Json> AuthorizeMessage(const std::string&, const SignalRequest&) const;
    std::shared_ptr<Session> FindSession(const std::string&, bool subscriber) const;
    std::shared_ptr<Session> SelectSession(const std::string&, const SignalRequest&, Json& error) const;
    Json HandleNegotiationReply(Session&, const SignalRequest&);
    Json HandleMediaOffer(const std::string&, const SignalRequest&);
    Json HandleMediaStop(const std::string&, const SignalRequest&);
    Json HandleAudioControl(const std::string&, const SignalRequest&);
    Json HandleSubscribe(const std::string&, const SignalRequest&, const std::string&);
    std::optional<sdp::SdpMedia> FindPublishedMedia(const std::string&, const std::string&) const;
    Json CreateSession(const std::string&, const std::string&, const std::vector<Subscription>& = {});
    Json JoinRoom(const std::string&, const ParticipantInit&);
    Json ResumeRoom(const std::string&, const std::string&, const std::string&, const std::string&,
                    const service::auth::AuthContext&);
    Json ListTracks(const std::string&) const;
    Json HandleParticipant(const std::string&, const SignalRequest&);
    Json ApplyParticipantCommand(std::shared_ptr<room::Room>, const std::string& actor, const SignalRequest&);
    Json ListParticipants(const std::shared_ptr<room::Room>&, const SignalRequest&) const;
    void RetireAssignment(const std::string& room, const std::string& assignment);
    void ObserveRoom(const std::shared_ptr<room::Room>&);
    void NotifyTrackChanges();
    void NotifyParticipantChanges();
    void ObserveMediaRegistry();
    void ReconcileMediaTracks();
    void LeaveRoom(const std::string&);
    void RemoveSession(const std::string&);
    void RemoveMediaSession(const std::string&, std::optional<bool> subscriber = std::nullopt);
    const std::shared_ptr<TaskScheduler> scheduler_;
    const config::AppConfig config_;
    const std::shared_ptr<media::IEncodedFramePublisher> publisher_;
    const std::shared_ptr<const service::auth::AuthService> auth_;
    WebRtcServiceStats& stats_;
    const std::shared_ptr<RoomCapacity> capacity_;
    Emit emit_;
    std::shared_ptr<network::UdpServer> udp_;
    std::shared_ptr<protocol::webrtc::WebRtcUdpMux> mux_;
    std::shared_ptr<DatagramTasks> datagrams_;
    uint16_t udp_port_ = 0;
    int network_family_ = 0;
    bool owns_network_ = true;
    std::unordered_map<std::string, MediaSessions> sessions_;
    std::unordered_map<std::string, std::unique_ptr<Membership>> memberships_;
    std::unordered_map<std::string, std::string> participant_connections_;
    std::unordered_map<std::string, std::shared_ptr<room::Room>> rooms_;
    std::unordered_map<std::string, std::unique_ptr<RoomObservation>> room_observations_;
    struct Assignment { std::string id; int64_t expires = 0; };
    struct RetiredAssignment { std::string room; int64_t expires = 0; bool reported = false; };
    std::unordered_map<std::string, Assignment> assignments_;
    std::unordered_map<std::string, RetiredAssignment> retired_assignments_;
    bool started_ = false;
    std::shared_ptr<std::atomic<bool>> core_tracks_dirty_ = std::make_shared<std::atomic<bool>>(false);
    media::EventBus::Subscription core_track_changed_;
    media::EventBus::Subscription core_stream_removed_;
};
}
