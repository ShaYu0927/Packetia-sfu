#ifndef PACKETIA_SERVER_WEBRTC_SERVICE_H_
#define PACKETIA_SERVER_WEBRTC_SERVICE_H_

#include "AppConfig.h"
#include "AuthService.h"
#include "SdpMode.h"
#include "signaling/WebRtcServiceStats.h"
#include "signaling/RoomOwner.h"
#include <functional>
#include <memory>
#include <string>
#include <vector>

class EventLoop;
class TaskScheduler;
namespace media { class IEncodedFramePublisher; }
namespace server::signaling
{
class RoomSignalHandler;
class SignalSessionCoordinator;
class WsSignalGateway;
}
namespace network { class UdpServer; }
namespace network::transport { class UdpSendQueue; }
namespace protocol::webrtc { class WebRtcUdpMux; }

namespace server
{
// 对外保留服务生命周期接口；具体信令与房间业务由独立模块承担。
class WebRtcService final : public std::enable_shared_from_this<WebRtcService>
{
public:
    WebRtcService(EventLoop* loop, config::AppConfig config,
                  std::shared_ptr<media::IEncodedFramePublisher> publisher,
                  std::function<bool(const service::auth::AuthContext&)> admission_guard = {});
    ~WebRtcService();
    WebRtcService(const WebRtcService&) = delete;
    WebRtcService& operator=(const WebRtcService&) = delete;

    bool Start();
    void Stop();
    bool Renegotiate(uint64_t sessionId, const std::vector<sdp::SdpMedia>& medias, std::string& error);
    std::size_t ParticipantCount();
    // 在 owner 上读取快照；累计计数跨重启保留，实时数量随生命周期更新。
    WebRtcServiceStats GetStats();
    // 仅供已认证的节点管理入口调用；等待发生在 API worker，owner 从不等待网络。
    signaling::Json ControlRoom(const std::string& room, const std::string& assignment, signaling::Json command);
    std::vector<signaling::RoomRelease> ReleasedRooms();

private:
    EventLoop* const loop_;
    const config::AppConfig config_;
    const std::shared_ptr<media::IEncodedFramePublisher> publisher_;
    const std::function<bool(const service::auth::AuthContext&)> admission_guard_;
    std::shared_ptr<TaskScheduler> scheduler_;
    std::vector<signaling::RoomOwner> room_owners_;
    std::shared_ptr<network::UdpServer> udp_;
    std::shared_ptr<protocol::webrtc::WebRtcUdpMux> mux_;
    std::shared_ptr<network::transport::UdpSendQueue> udp_sends_;
    std::shared_ptr<signaling::SignalSessionCoordinator> coordinator_;
    std::shared_ptr<signaling::WsSignalGateway> gateway_;
    WebRtcServiceStats stats_;
    uint32_t timer_ = 0;
    bool started_ = false;
};
}
#endif
