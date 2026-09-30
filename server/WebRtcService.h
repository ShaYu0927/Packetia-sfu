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

namespace server
{
// Assembles the existing UDP, WebSocket and media services on one I/O owner.
// Start/Stop are called by ServerApp's serialized lifecycle controller.
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
    // Control-thread API for an existing conference participant. MIDs identify
    // tracks; omitted MIDs are stopped. Accepted offers are queued on its WS.
    // The peer answers with the supplied negotiation_id; media stays active
    // until that answer is validated. Current service policy is publish-only.
    bool Renegotiate(uint64_t sessionId, const std::vector<sdp::SdpMedia>& medias, std::string& error);

private:
    struct Session;
    std::string OnMessage(const std::string& connection, const std::string& message);
    std::string CreateSession(const std::string& connection, const std::string& offer);
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
    uint32_t timer_ = 0;
    bool started_ = false;
};
}

#endif // PACKETIA_SERVER_WEBRTC_SERVICE_H_
