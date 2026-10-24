#pragma once
#include "IService.h"
#include "RuntimeMetrics.h"
#include "SystemResourceCollector.h"
#include "ThreadManager.h"
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <memory>
#include <mutex>

namespace service
{
struct MonitorSnapshot
{
    monitoring::RuntimeSnapshot runtime;
    monitoring::SystemResources resources;
};
class MonitorService final : public IService
{
public:
    explicit MonitorService(std::shared_ptr<monitoring::MetricRegistry> registry,
                            std::chrono::milliseconds interval = std::chrono::seconds(1));
    ~MonitorService() override;
    bool Init() override;
    bool Start() override;
    void Stop() override;
    void Shutdown() override;
    ServiceType Type() const override { return ServiceType::Monitor; }
    ServiceState State() const override { return state_.load(); }
    ServiceHealth Health() const override;
    std::shared_ptr<const MonitorSnapshot> Snapshot() const;
private:
    void Collect();
    void Run();
    std::shared_ptr<monitoring::MetricRegistry> registry_;
    const std::chrono::milliseconds interval_;
    monitoring::SystemResourceCollector resources_;
    std::shared_ptr<const MonitorSnapshot> snapshot_;
    std::mutex lifecycle_mutex_, wait_mutex_;
    std::condition_variable wake_;
    bool stopping_ = false;
    utils::ManagedThread thread_;
    std::atomic<ServiceState> state_{ServiceState::Created};
    std::atomic<int> error_{0};
};
}
