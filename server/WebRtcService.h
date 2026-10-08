#ifndef PACKETIA_SERVER_WEBRTC_SERVICE_H_
#define PACKETIA_SERVER_WEBRTC_SERVICE_H_

#include "config/AppConfig.h"
#include "SdpMode.h"

#include <cstdint>
#include <memory>
#include <string>
#include <unordered_map>

class EventLoop;
class TaskScheduler;
namespace network
{
class UdpServer;
namespace websocket { class WsServer; }
}
namespace media { class IEncodedFramePublisher; }
namespace protocol::webrtc { class WebRtcUdpMux; }
namespace room { class Room; }

namespace server
{
class WebRtcService final : public std::enable_shared_from_this<WebRtcService>
{
public:
    WebRtcService(EventLoop* loop, config::AppConfig config,
                  std::shared_ptr<media::IEncodedFramePublisher> publisher);
    ~WebRtcService();
    WebRtcService(const WebRtcService&) = delete;
    WebRtcService& operator=(const WebRtcService&) = delete;

    bool Start();
    void Stop();
    bool Renegotiate(uint64_t sessionId, const std::vector<sdp::SdpMedia>& medias, std::string& error);

private:
    struct Session;
    struct Membership;
    struct Subscription;
    std::string OnMessage(const std::string& connection, const std::string& message);
    std::string CreateSession(const std::string& connection, const std::string& offer,
                              const std::vector<Subscription>& subscriptions = {});
    std::string JoinRoom(const std::string& connection, const std::string& roomId);
    std::string ListTracks(const std::string& connection) const;
    void LeaveRoom(const std::string& connection);
    std::string UpdateSession(Session& session, const std::string& offer);
    void RemoveSession(const std::string& connection);
    bool Tick();
    void StopOnOwner();

    EventLoop* const loop_;
    const config::AppConfig config_;
    const std::shared_ptr<media::IEncodedFramePublisher> publisher_;
    std::shared_ptr<TaskScheduler> scheduler_;
    std::shared_ptr<network::UdpServer> udp_;
    std::shared_ptr<network::websocket::WsServer> ws_;
    std::shared_ptr<protocol::webrtc::WebRtcUdpMux> mux_;
    std::unordered_map<std::string, std::unique_ptr<Session>> sessions_;
    std::unordered_map<std::string, std::unique_ptr<Membership>> memberships_;
    std::unordered_map<std::string, std::shared_ptr<room::Room>> rooms_;
    uint32_t timer_ = 0;
    bool started_ = false;
};
}

#endif // PACKETIA_SERVER_WEBRTC_SERVICE_H_
