#pragma once
#include "Coordinator.h"
#include "NodeLease.h"
#include "ThreadManager.h"
#include "WebRtcService.h"
#include <condition_variable>
#include <functional>
#include <mutex>

namespace server::cluster
{
class NodeClient final
{
public:
    NodeClient(service::cluster::Node node, std::string control_url, std::string credential,
               std::shared_ptr<service::cluster::NodeLease> lease, std::shared_ptr<WebRtcService> rtc);
    ~NodeClient();
    bool Start();
    void Stop() noexcept;
private:
    long Send(bool registration);
    void Run();
    void Reclaim(const nlohmann::json& reply, int64_t started);
    const service::cluster::Node node_;
    const std::string control_url_, credential_;
    const std::shared_ptr<service::cluster::NodeLease> lease_;
    const std::shared_ptr<WebRtcService> rtc_;
    std::mutex mutex_;
    std::condition_variable wake_;
    bool stopping_ = false;
    utils::ManagedThread thread_;
};
}
