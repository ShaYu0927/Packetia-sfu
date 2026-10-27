#pragma once
#include "AppConfig.h"
#include "SignalSessionCoordinator.h"
#include "WsServer.h"
#include <unordered_map>

namespace server::signaling
{
// 只拥有连接与编解码边界，不接触房间、端点和具体协商对象。
class WsSignalGateway : public std::enable_shared_from_this<WsSignalGateway>
{
public:
    WsSignalGateway(std::shared_ptr<TaskScheduler> owner, const config::AppConfig& config,
                    std::shared_ptr<SignalSessionCoordinator> coordinator);
    bool Start(const std::string& ip, uint16_t port);
    void Tick();
    void Stop();
private:
    void Open(const network::websocket::WsConnectionInfo& info);
    void Receive(const std::string& connection, const std::string& text);
    void Write(const SignalKey& key, SignalResponse response);
    void Closed(const std::string& connection);
    const std::shared_ptr<TaskScheduler> owner_;
    const std::shared_ptr<SignalSessionCoordinator> coordinator_;
    network::websocket::WsServer ws_;
    std::unordered_map<std::string, std::shared_ptr<SignalSession>> sessions_;
};
}
