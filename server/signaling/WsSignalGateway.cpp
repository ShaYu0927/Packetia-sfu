#include "WsSignalGateway.h"
#include <atomic>

namespace server::signaling
{
namespace
{
network::websocket::WsServerOptions Options(const config::AppConfig& config)
{
    network::websocket::WsServerOptions options;
    options.limits.max_message_bytes = kMaxMessageBytes;
    options.idle_ping_seconds = config.webrtc.signaling_ping_seconds;
    options.idle_timeout_seconds = config.webrtc.signaling_idle_timeout_seconds;
    return options;
}
// 跨服务重启也不复用连接代次；旧任务不能命中新建的同名 WebSocket。
std::atomic<uint64_t> next_generation{0};
}
WsSignalGateway::WsSignalGateway(std::shared_ptr<TaskScheduler> owner, const config::AppConfig& config,
    std::shared_ptr<SignalSessionCoordinator> coordinator)
    : owner_(std::move(owner)), coordinator_(std::move(coordinator)), ws_(owner_, Options(config)) {}
bool WsSignalGateway::Start(const std::string& ip, uint16_t port)
{
    const auto weak = weak_from_this();
    ws_.SetOnOpen([weak](const auto& info) { if (auto self = weak.lock()) self->Open(info); });
    ws_.SetOnMessage([weak](const std::string& id, const std::string& text)
    { if (auto self = weak.lock()) self->Receive(id, text); return std::string{}; });
    ws_.SetOnClose([weak](const std::string& id) { if (auto self = weak.lock()) self->Closed(id); });
    return ws_.Start(ip, port);
}
void WsSignalGateway::Open(const network::websocket::WsConnectionInfo& info)
{
    const SignalKey key{info.connId, ++next_generation};
    const auto weak = weak_from_this();
    const std::weak_ptr<TaskScheduler> owner = owner_;
    auto session = coordinator_->Open(key,
        [owner](std::function<void()> task) { const auto loop = owner.lock(); return loop && loop->Post(std::move(task)); },
        [weak, key](SignalResponse response) { if (auto self = weak.lock()) self->Write(key, std::move(response)); });
    if (!session) { ws_.CloseConnection(info.connId); return; }
    try { sessions_.emplace(info.connId, std::move(session)); }
    catch (...) { coordinator_->Closed(key); throw; }
}
void WsSignalGateway::Receive(const std::string& connection, const std::string& text)
{
    const auto found = sessions_.find(connection);
    if (found == sessions_.end()) { ws_.CloseConnection(connection); return; }
    auto session = found->second;
    SignalRequest request;
    Json error;
    if (!DecodeRequest(text, request, error)) request.rejection = std::move(error);
    const auto result = session->Request(std::move(request), text.size());
    // 关键信令入队失败时终止连接，走确定的断连清理，不静默丢弃后继续会话。
    if (result != SignalResult::Accepted) ws_.CloseConnection(connection);
}
void WsSignalGateway::Write(const SignalKey& key, SignalResponse response)
{
    const auto found = sessions_.find(key.connection);
    if (found == sessions_.end() || !(found->second->Key() == key) || found->second->IsCancelled()) return;
    if (!ws_.SendText(key.connection, response.body.dump()))
    { ws_.CloseConnection(key.connection); return; }
    // 两者共用 owner；确认失败立即清空 socket 队列，晚到的首响不会写出。
    if (response.initial && !coordinator_->ConfirmInitial(key, response.admission_generation))
    { ws_.CloseConnection(key.connection); return; }
    if (response.close_after && !ws_.DrainConnection(key.connection)) ws_.CloseConnection(key.connection);
}
void WsSignalGateway::Closed(const std::string& connection)
{
    const auto found = sessions_.find(connection);
    if (found == sessions_.end()) return;
    const auto key = found->second->Key();
    found->second->Cancel();
    sessions_.erase(found);
    coordinator_->Closed(key);
}
void WsSignalGateway::Tick()
{
    for (const auto& item : sessions_) if (item.second->IsCancelled()) ws_.CloseConnection(item.first);
}
void WsSignalGateway::Stop()
{
    ws_.Stop();
    while (!sessions_.empty()) Closed(sessions_.begin()->first);
}
}
