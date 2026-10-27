#ifndef PACKETIA_SERVER_WEBRTC_SERVICE_H_
#define PACKETIA_SERVER_WEBRTC_SERVICE_H_

#include "AppConfig.h"
#include "AuthService.h"
#include "SdpMode.h"
#include "signaling/WebRtcServiceStats.h"
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

private:
    void StopOnOwner();
    EventLoop* const loop_;
    const config::AppConfig config_;
    const std::shared_ptr<media::IEncodedFramePublisher> publisher_;
    const std::function<bool(const service::auth::AuthContext&)> admission_guard_;
    std::shared_ptr<TaskScheduler> scheduler_;
    std::shared_ptr<signaling::RoomSignalHandler> room_;
    std::shared_ptr<signaling::SignalSessionCoordinator> coordinator_;
    std::shared_ptr<signaling::WsSignalGateway> gateway_;
    WebRtcServiceStats stats_;
    uint32_t timer_ = 0;
    bool started_ = false;
};
}
#endif
