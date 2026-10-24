#include "MonitorService.h"
#include <stdexcept>
#include <utility>

namespace service
{
MonitorService::MonitorService(std::shared_ptr<monitoring::MetricRegistry> registry, std::chrono::milliseconds interval)
    : registry_(std::move(registry)), interval_(interval)
{
    if (!registry_ || interval_.count() < 1) throw std::invalid_argument("invalid monitor registry or interval");
}
MonitorService::~MonitorService() { Stop(); }
bool MonitorService::Init()
{
    std::lock_guard<std::mutex> lock(lifecycle_mutex_);
    if (state_ == ServiceState::Created) state_ = ServiceState::Initialized;
    return state_ == ServiceState::Initialized;
}
bool MonitorService::Start()
{
    std::lock_guard<std::mutex> lock(lifecycle_mutex_);
    if (state_ == ServiceState::Running) return true;
    if (state_ != ServiceState::Initialized && state_ != ServiceState::Stopped) return false;
    state_ = ServiceState::Starting;
    try
    {
        resources_ = monitoring::SystemResourceCollector{};
        Collect();
        { std::lock_guard<std::mutex> wait_lock(wait_mutex_); stopping_ = false; }
        thread_ = utils::ThreadManager::Instance().Start(
            {"monitor", "monitor-sampler", utils::ThreadModel::Dedicated}, [this] { Run(); },
            [this] {
                { std::lock_guard<std::mutex> wait_lock(wait_mutex_); stopping_ = true; }
                wake_.notify_all();
            });
        error_ = 0;
        state_ = ServiceState::Running;
        return true;
    }
    catch (...) { error_ = 1; state_ = ServiceState::Failed; return false; }
}
void MonitorService::Stop()
{
    std::lock_guard<std::mutex> lock(lifecycle_mutex_);
    if (state_ == ServiceState::Created || state_ == ServiceState::Initialized || state_ == ServiceState::Stopped) return;
    state_ = ServiceState::Stopping;
    thread_.RequestStop();
    thread_.Join();
    state_ = ServiceState::Stopped;
}
void MonitorService::Shutdown()
{
    Stop();
    std::lock_guard<std::mutex> lock(lifecycle_mutex_);
    std::atomic_store(&snapshot_, std::shared_ptr<const MonitorSnapshot>{});
    state_ = ServiceState::Created; error_ = 0;
}
ServiceHealth MonitorService::Health() const
{ return {state_ == ServiceState::Running && error_ == 0, Type(), State(), error_.load()}; }
std::shared_ptr<const MonitorSnapshot> MonitorService::Snapshot() const
{ return std::atomic_load(&snapshot_); }
void MonitorService::Collect()
{
    auto next = std::make_shared<MonitorSnapshot>();
    next->runtime = registry_->Collect();
    next->resources = resources_.Collect(next->runtime.captured_ns);
    std::atomic_store(&snapshot_, std::shared_ptr<const MonitorSnapshot>(std::move(next)));
}
void MonitorService::Run()
{
    std::unique_lock<std::mutex> lock(wait_mutex_);
    while (!wake_.wait_for(lock, interval_, [this] { return stopping_; }))
    {
        lock.unlock();
        try { Collect(); error_ = 0; }
        catch (...) { error_ = 2; } // Preserve the last published snapshot on sampling failure.
        lock.lock();
    }
}
}
