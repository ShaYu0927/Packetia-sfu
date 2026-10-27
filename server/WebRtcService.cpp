#include "WebRtcService.h"
#include "signaling/RoomSignalHandler.h"
#include "signaling/SignalSessionCoordinator.h"
#include "signaling/WsSignalGateway.h"
#include "EventLoop.h"
#include "JwtAuthenticator.h"
#include "KeyProvider.h"
#include "RoomAccessPolicy.h"
#include "logger.h"
#include <stdexcept>
#include <utility>

namespace server
{
namespace auth = service::auth;
WebRtcService::WebRtcService(EventLoop* loop, config::AppConfig config,
    std::shared_ptr<media::IEncodedFramePublisher> publisher,
    std::function<bool(const auth::AuthContext&)> admission_guard)
    : loop_(loop), config_(std::move(config)), publisher_(std::move(publisher)),
      admission_guard_(std::move(admission_guard)) {}
WebRtcService::~WebRtcService() { Stop(); }

bool WebRtcService::Start()
{
    if (started_) return scheduler_ && !scheduler_->IsStopped();
    if (!loop_ || weak_from_this().expired())
    {
        LOG_ERROR("WebRtcService Start: requires an event loop and shared service ownership");
        return false;
    }
    try { config_.Validate(); }
    catch (const std::invalid_argument& error)
    { LOG_ERROR("WebRtcService Start: ", error.what()); return false; }
    if (!config_.webrtc.enabled || config_.webrtc.api_keys.empty() || !config_.webrtc.max_sessions)
    { LOG_ERROR("WebRtcService Start: enable WebRTC with API keys and a positive session limit"); return false; }
    scheduler_ = loop_->GetTaskScheduler();
    if (!scheduler_ || scheduler_->IsStopped())
    { LOG_ERROR("WebRtcService Start: event loop is not running"); return false; }
    bool result = false;
    try
    {
        scheduler_->Invoke([&]
        {
            auto auth = std::make_shared<auth::AuthService>(
                std::make_shared<auth::JwtAuthenticator>(std::make_shared<auth::StaticKeyProvider>(config_.webrtc.api_keys)),
                std::make_shared<auth::RoomAccessPolicy>());
            room_ = std::make_shared<signaling::RoomSignalHandler>(scheduler_, config_, publisher_, auth, stats_);
            if (!room_->Start()) return;
            coordinator_ = std::make_shared<signaling::SignalSessionCoordinator>(scheduler_, room_,
                signaling::AdmissionValidator(auth, admission_guard_), stats_, config_.webrtc.admission_timeout_ms);
            coordinator_->Start();
            gateway_ = std::make_shared<signaling::WsSignalGateway>(scheduler_, config_, coordinator_);
            if (!gateway_->Start(config_.listen_ip, config_.websocket_port)) return;
            const auto weak = weak_from_this();
            timer_ = scheduler_->AddTimer([weak]
            {
                const auto self = weak.lock();
                if (!self || !self->started_) return false;
                self->coordinator_->Tick();
                self->gateway_->Tick();
                return true;
            }, 100);
            started_ = result = timer_ != 0;
        });
    }
    catch (...) { Stop(); throw; }
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
    // 先断开网络和取消转发，再清理房间、媒体及 UDP；排队的弱引用任务随后无效。
    if (gateway_) gateway_->Stop();
    if (coordinator_) coordinator_->Stop();
    if (room_) room_->Stop();
    gateway_.reset(); coordinator_.reset(); room_.reset();
}
std::size_t WebRtcService::ParticipantCount()
{
    std::size_t count = 0;
    if (scheduler_) scheduler_->Invoke([&] { if (room_) count = room_->ParticipantCount(); });
    return count;
}
WebRtcServiceStats WebRtcService::GetStats()
{
    WebRtcServiceStats snapshot;
    if (scheduler_) scheduler_->Invoke([&] { snapshot = coordinator_ ? coordinator_->GetStats() : stats_; });
    else snapshot = stats_;
    return snapshot;
}
bool WebRtcService::Renegotiate(uint64_t id, const std::vector<sdp::SdpMedia>& medias, std::string& error)
{
    bool result = false;
    if (!scheduler_) { error = "WebRTC service is stopped"; return false; }
    scheduler_->Invoke([&]
    {
        if (!started_ || !room_) { error = "WebRTC service is stopped"; return; }
        result = room_->Renegotiate(id, medias, error);
    });
    return result;
}
}
