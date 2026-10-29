#include "WebRtcService.h"
#include "signaling/RoomSignalHandler.h"
#include "signaling/SignalSessionCoordinator.h"
#include "signaling/WsSignalGateway.h"
#include "EventLoop.h"
#include "JwtAuthenticator.h"
#include "KeyProvider.h"
#include "RoomAccessPolicy.h"
#include "UdpServer.h"
#include "UdpSendQueue.h"
#include "WebRtcUdpMux.h"
#include "logger.h"
#include <algorithm>
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
    auto executors = loop_->GetTaskSchedulers();
    executors.erase(std::remove(executors.begin(), executors.end(), scheduler_), executors.end());
    if (executors.empty()) executors.push_back(scheduler_);
    bool networkReady = false;
    uint16_t port = 0;
    int addressFamily = 0;
    try
    {
        auto udp_budget = std::make_shared<utils::ChannelBudget>();
        scheduler_->Invoke([&] {
            udp_ = std::make_shared<network::UdpServer>(scheduler_);
            if (!udp_->Start(config_.listen_ip, config_.udp_port, false)) return;
            const auto local = udp_->LocalAddress();
            port = local.Port(); addressFamily = local.ss.ss_family;
            udp_sends_ = std::make_shared<network::transport::UdpSendQueue>(scheduler_, udp_, udp_budget);
            mux_ = std::make_shared<protocol::webrtc::WebRtcUdpMux>(udp_, udp_sends_->MakeSender());
            udp_->SetHandler(mux_);
            networkReady = port != 0;
        });
        if (!networkReady) { Stop(); return false; }
        auto auth = std::make_shared<auth::AuthService>(
            std::make_shared<auth::JwtAuthenticator>(std::make_shared<auth::StaticKeyProvider>(config_.webrtc.api_keys)),
            std::make_shared<auth::RoomAccessPolicy>());
        auto capacity = std::make_shared<signaling::RoomCapacity>(config_.webrtc.max_sessions);
        auto control_budget = std::make_shared<utils::ChannelBudget>();
        for (const auto& executor : executors)
        {
            signaling::RoomOwner room{executor, {}, std::make_shared<WebRtcServiceStats>()};
            room.handler = std::make_shared<signaling::RoomSignalHandler>(executor, config_, publisher_, auth, *room.stats, capacity);
            const std::weak_ptr<TaskScheduler> control_owner = executor;
            const auto handler = room.handler;
            room.controls = std::make_shared<signaling::RoomControlChannel>(
                [control_owner](auto task) { const auto owner = control_owner.lock(); return owner && owner->Post(std::move(task)); },
                [handler](signaling::RoomControlRequest request) {
                    signaling::Json result;
                    if (request.cancelled->load() || Timestamp::NowMs() >= request.deadline_ms)
                        result = {{"type", "error"}, {"code", "unavailable"}, {"error", "Room command deadline exceeded"}};
                    else try { result = handler->ControlRoom(request.room_id, request.assignment_id, request.command); }
                    catch (...) { result = signaling::Error("Invalid room management command"); }
                    request.result->set_value(std::move(result));
                }, control_budget, utils::ChannelLimits{4 * 1024 * 1024, 64});
            room_owners_.push_back(room);
            bool ready = false;
            executor->Invoke([&] {
                room.handler->AttachNetwork(udp_, mux_, port, addressFamily, udp_budget);
                ready = room.handler->Start();
            });
            if (!ready) { Stop(); return false; }
        }
        bool ready = false;
        scheduler_->Invoke([&] {
            coordinator_ = std::make_shared<signaling::SignalSessionCoordinator>(scheduler_, room_owners_,
                signaling::AdmissionValidator(auth, admission_guard_), stats_, config_.webrtc.admission_timeout_ms);
            coordinator_->Start();
            gateway_ = std::make_shared<signaling::WsSignalGateway>(scheduler_, config_, coordinator_);
            if (!gateway_->Start(config_.listen_ip, config_.websocket_port)) return;
            const auto weak = weak_from_this();
            timer_ = scheduler_->AddTimer([weak] {
                const auto self = weak.lock();
                if (!self || !self->started_) return false;
                self->coordinator_->Tick();
                self->gateway_->Tick();
                return true;
            }, 100);
            started_ = ready = timer_ != 0;
        });
        if (!ready) Stop();
        return ready;
    }
    catch (...) { Stop(); throw; }
}
void WebRtcService::Stop()
{
    // 生命周期调用由控制线程发起；先取消接入，再逐个等待房间清理，最后关闭网络。
    auto stopFront = [this] {
        started_ = false;
        if (timer_ && scheduler_) scheduler_->RemoveTimer(timer_);
        timer_ = 0;
        if (gateway_) gateway_->Stop();
        if (coordinator_) coordinator_->Stop();
    };
    if (scheduler_) scheduler_->Invoke(stopFront); else stopFront();
    for (std::size_t i = 0; i < room_owners_.size(); ++i)
    {
        auto& room = room_owners_[i];
        if (room.controls) room.controls->Cancel();
        room.executor->Invoke([&] { room.handler->Stop(); });
        const auto counters = *room.stats;
        if (scheduler_) scheduler_->Invoke([&, i, counters] {
            if (coordinator_) coordinator_->Refresh(i, 0, 0, counters);
        });
    }
    auto stopNetwork = [this] {
        if (coordinator_) stats_ = coordinator_->GetStats();
        if (udp_sends_) udp_sends_->Cancel();
        if (mux_) mux_->Close();
        if (udp_) { udp_->SetHandler({}); udp_->Stop(); }
        gateway_.reset(); coordinator_.reset(); udp_sends_.reset(); mux_.reset(); udp_.reset();
    };
    if (scheduler_) scheduler_->Invoke(stopNetwork); else stopNetwork();
    room_owners_.clear();
    scheduler_.reset();
}
std::size_t WebRtcService::ParticipantCount()
{
    std::size_t count = 0;
    if (scheduler_) scheduler_->Invoke([&] { if (coordinator_) count = coordinator_->ParticipantCount(); });
    return count;
}
WebRtcServiceStats WebRtcService::GetStats()
{
    WebRtcServiceStats snapshot;
    if (scheduler_) scheduler_->Invoke([&] { snapshot = coordinator_ ? coordinator_->GetStats() : stats_; });
    else snapshot = stats_;
    return snapshot;
}
signaling::Json WebRtcService::ControlRoom(const std::string& roomId, const std::string& assignment, signaling::Json command)
{
    const auto unavailable = [] { return signaling::Json{{"type", "error"}, {"code", "unavailable"},
        {"error", "Room command unavailable or timed out; execution may have started"}}; };
    if (!started_ || room_owners_.empty()) return unavailable();
    auto& room = room_owners_[signaling::SignalSessionCoordinator::SelectOwner(roomId, room_owners_.size())];
    // 同一执行器上的调用不允许阻塞等待自己的队列。
    if (room.executor->IsCurrentThread()) return room.handler->ControlRoom(roomId, assignment, command);
    const auto bytes = command.dump().size();
    if (bytes > signaling::kMaxMessageBytes) return signaling::Error("Room command is too large");
    const auto promise = std::make_shared<std::promise<signaling::Json>>();
    auto future = promise->get_future();
    const auto cancelled = std::make_shared<std::atomic<bool>>(false);
    signaling::RoomControlRequest request{roomId, assignment, std::move(command), Timestamp::NowMs() + 1000, cancelled, promise};
    if (room.controls->Send(std::move(request), bytes) != utils::ChannelResult::Accepted) return unavailable();
    if (future.wait_for(std::chrono::milliseconds(1000)) != std::future_status::ready)
    { cancelled->store(true); return unavailable(); }
    try { return future.get(); } catch (...) { return unavailable(); }
}
std::vector<signaling::RoomRelease> WebRtcService::ReleasedRooms()
{
    if (!scheduler_) return {};
    const auto result = std::make_shared<std::promise<std::vector<signaling::RoomRelease>>>();
    auto future = result->get_future();
    const auto coordinator = coordinator_;
    auto read = [result, coordinator] { result->set_value(coordinator ? coordinator->ReleasedRooms() : std::vector<signaling::RoomRelease>{}); };
    if (scheduler_->IsCurrentThread()) read();
    else if (!scheduler_->Post(std::move(read))) return {};
    return future.wait_for(std::chrono::milliseconds(1000)) == std::future_status::ready ? future.get() : std::vector<signaling::RoomRelease>{};
}
bool WebRtcService::Renegotiate(uint64_t id, const std::vector<sdp::SdpMedia>& medias, std::string& error)
{
    // 控制接口保持同步签名；正常信令和媒体收发均使用异步通道。
    if (!started_) { error = "WebRTC service is stopped"; return false; }
    for (const auto& room : room_owners_)
    {
        bool result = false;
        room.executor->Invoke([&] { result = room.handler->Renegotiate(id, medias, error); });
        if (result || error != "Unknown WebRTC session") return result;
    }
    error = "Unknown WebRTC session";
    return false;
}
}
