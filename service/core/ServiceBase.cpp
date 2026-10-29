#include "ServiceBase.h"

namespace service
{
ServiceState ServiceBase::State() const { return state_.load(); }
ServiceHealth ServiceBase::Health() const
{
    const auto state = State();
    const auto error = error_code_.load();
    return {state == ServiceState::Running && error == 0, Type(), state, error};
}
void ServiceBase::SetFailed(int error_code)
{
    error_code_ = error_code ? error_code : -1;
    state_ = ServiceState::Failed;
}
bool ServiceBase::Init()
{
    std::lock_guard<std::mutex> lock(lifecycle_mutex_);
    const auto state = State();
    if (state == ServiceState::Initialized || state == ServiceState::Running || state == ServiceState::Stopped) return true;
    if (state != ServiceState::Created) return false;
    error_code_ = 0;
    try
    {
        if (OnInit() && State() != ServiceState::Failed)
        { state_ = ServiceState::Initialized; return true; }
    }
    catch (...) {}
    // 失败服务自身也要回滚，不能只依赖 Manager 回滚此前成功的服务。
    try { OnShutdown(); } catch (...) {}
    SetFailed(error_code_.load());
    return false;
}
bool ServiceBase::Start()
{
    std::lock_guard<std::mutex> lock(lifecycle_mutex_);
    if (State() == ServiceState::Running) return true;
    if (State() != ServiceState::Initialized && State() != ServiceState::Stopped) return false;
    error_code_ = 0;
    state_ = ServiceState::Starting;
    try
    {
        if (OnStart() && State() != ServiceState::Failed)
        { state_ = ServiceState::Running; return true; }
    }
    catch (...) {}
    state_ = ServiceState::Stopping;
    try { OnStop(); } catch (...) {}
    SetFailed(error_code_.load());
    return false;
}
void ServiceBase::Stop()
{
    std::lock_guard<std::mutex> lock(lifecycle_mutex_);
    if (State() == ServiceState::Created || State() == ServiceState::Stopped) return;
    const bool failed = State() == ServiceState::Failed;
    state_ = ServiceState::Stopping;
    try
    {
        OnStop();
        if (failed || State() == ServiceState::Failed) state_ = ServiceState::Failed;
        else state_ = ServiceState::Stopped;
    }
    catch (...) { SetFailed(error_code_.load()); }
}
void ServiceBase::Shutdown()
{
    std::lock_guard<std::mutex> lock(lifecycle_mutex_);
    if (State() == ServiceState::Created) return;
    state_ = ServiceState::Stopping;
    // OnStop 和 OnShutdown 均需幂等；释放资源必须在停止使用资源之后。
    try { OnStop(); }
    catch (...) { SetFailed(error_code_.load()); return; }
    if (State() == ServiceState::Failed) return;
    try
    {
        OnShutdown();
        if (State() == ServiceState::Failed) return;
        error_code_ = 0;
        state_ = ServiceState::Created;
    }
    catch (...) { SetFailed(error_code_.load()); }
}
}
