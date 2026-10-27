#pragma once
#include "AppConfig.h"
#include "AdmissionValidator.h"
#include "WebRtcServiceStats.h"
#include "SdpMode.h"
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
                      std::shared_ptr<const service::auth::AuthService> auth, WebRtcServiceStats& stats);
    ~RoomSignalHandler();
    bool Start();
    void Stop();
    void SetEmitter(Emit emit) { emit_ = std::move(emit); }
    Json Handle(const std::string& connection, const SignalRequest& request, const ParticipantInit* init = nullptr);
    void OnSignalingClosed(const std::string& connection);
    void Tick();
    bool Renegotiate(uint64_t session, const std::vector<sdp::SdpMedia>& medias, std::string& error);
    std::size_t ParticipantCount() const { return memberships_.size(); }
    std::size_t ReconnectingCount() const;
    bool HasMember(const std::string& connection) const { return memberships_.count(connection) != 0; }
    void RejectNewAdmission(const std::string& connection) { RemoveSession(connection); }
private:
    using Session = server::WebRtcSessionContext;
    struct Membership;
    struct Subscription;
    struct RoomObservation;
    Json DispatchMessage(const std::string&, const SignalRequest&, const ParticipantInit*);
    std::optional<Json> AuthorizeMessage(const std::string&, const SignalRequest&) const;
    Json HandleNegotiationReply(Session&, const SignalRequest&);
    Json HandleMediaOffer(const std::string&, const SignalRequest&);
    Json HandleSubscribe(const std::string&, const SignalRequest&, const std::string&);
    std::optional<sdp::SdpMedia> FindPublishedMedia(const std::string&, const std::string&) const;
    Json CreateSession(const std::string&, const std::string&, const std::vector<Subscription>& = {});
    Json JoinRoom(const std::string&, const std::string&, service::auth::AuthContext);
    Json ResumeRoom(const std::string&, const std::string&, const std::string&, const std::string&,
                    const service::auth::AuthContext&);
    Json ListTracks(const std::string&) const;
    void ObserveRoom(const std::shared_ptr<room::Room>&);
    void NotifyTrackChanges();
    void LeaveRoom(const std::string&);
    void RemoveSession(const std::string&);
    void RemoveMediaSession(const std::string&);
    const std::shared_ptr<TaskScheduler> scheduler_;
    const config::AppConfig config_;
    const std::shared_ptr<media::IEncodedFramePublisher> publisher_;
    const std::shared_ptr<const service::auth::AuthService> auth_;
    WebRtcServiceStats& stats_;
    Emit emit_;
    std::shared_ptr<network::UdpServer> udp_;
    std::shared_ptr<protocol::webrtc::WebRtcUdpMux> mux_;
    std::unordered_map<std::string, std::shared_ptr<Session>> sessions_;
    std::unordered_map<std::string, std::unique_ptr<Membership>> memberships_;
    std::unordered_map<std::string, std::string> participant_connections_;
    std::unordered_map<std::string, std::shared_ptr<room::Room>> rooms_;
    std::unordered_map<std::string, std::unique_ptr<RoomObservation>> room_observations_;
    bool started_ = false;
};
}
