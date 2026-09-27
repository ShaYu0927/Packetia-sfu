#include "ServerLauncher.h"
#include "log/logger.h"
#include "service/core/IService.h"

#include <algorithm>
#include <exception>
#include <stdexcept>

namespace server
{
namespace
{
void LogFailure(const std::string& name, const char* operation,
    const char* reason) noexcept
{
    try
    {
        LOG_ERROR("[ServerLauncher]", operation, "failed, name=", name,
            "reason=", reason);
    }
    catch (...)
    {
        // Logging must not interrupt rollback or destructor cleanup.
    }
}

template <typename Callback>
bool RunCleanup(const std::string& name, const char* operation,
    Callback&& callback) noexcept
{
    try
    {
        callback();
        return true;
    }
    catch (const std::exception& error)
    {
        LogFailure(name, operation, error.what());
    }
    catch (...)
    {
        LogFailure(name, operation, "unknown exception");
    }
    return false;
}
} // namespace

ServerLauncher::~ServerLauncher() noexcept
{
    StopAll();
}

void ServerLauncher::AddService(std::string name,
    std::shared_ptr<service::IService> instance, bool enabled)
{
    if (!instance)
    {
        throw std::invalid_argument("cannot register a null service");
    }

    AddCustomService(name,
        [instance] { return instance->Init() && instance->Start(); },
        [instance, name] {
            const bool stopped = RunCleanup(name, "stop", [&] { instance->Stop(); });
            const bool shutdown = RunCleanup(name, "shutdown", [&] { instance->Shutdown(); });
            if (!stopped || !shutdown) throw std::runtime_error("service cleanup failed");
        }, enabled);
}

void ServerLauncher::AddCustomService(const std::string& name,
    std::function<bool()> start, std::function<void()> stop, bool enabled)
{
    if (state_ != State::Stopped)
    {
        throw std::logic_error("cannot register services during an active lifecycle");
    }
    if (name.empty() || !start || !stop)
    {
        throw std::invalid_argument("services require a name and start/stop callbacks");
    }
    if (std::any_of(services_.begin(), services_.end(),
        [&name](const ServiceItem& item) { return item.name == name; }))
    {
        throw std::invalid_argument("duplicate service name: " + name);
    }

    services_.push_back({name, std::move(start), std::move(stop), enabled, false});
}

bool ServerLauncher::StartService(ServiceItem& item)
{
    // Include a partial start in cleanup.
    item.active = true;
    try {
        if (item.start()) return true;
        LogFailure(item.name, "start", "returned false");
    } catch (const std::exception& error) {
        LogFailure(item.name, "start", error.what());
    } catch (...) {
        LogFailure(item.name, "start", "unknown exception");
    }
    return false;
}

bool ServerLauncher::IsEnabled(const std::string& name) const
{
    const auto it = std::find_if(services_.begin(), services_.end(),
        [&](const ServiceItem& item) { return item.name == name; });
    return it != services_.end() && it->enabled;
}

bool ServerLauncher::SetEnabled(const std::string& name, bool enabled)
{
    if (state_ != State::Stopped && state_ != State::Running) return false;
    const auto it = std::find_if(services_.begin(), services_.end(),
        [&](const ServiceItem& item) { return item.name == name; });
    if (it == services_.end()) return false;
    if (state_ == State::Stopped) {
        it->enabled = enabled;
        return true;
    }
    if (it->enabled == enabled && it->active == enabled) return true;
    state_ = State::Switching;
    bool success = true;
    if (enabled) {
        // Retry any unfinished cleanup from a previous failed start first.
        if (it->active) {
            success = RunCleanup(it->name, "stop", it->stop);
            if (success) it->active = false;
        }
        if (success) {
            success = StartService(*it);
            if (!success && RunCleanup(it->name, "stop", it->stop)) it->active = false;
        }
    } else if (it->active) {
        success = RunCleanup(it->name, "stop", it->stop);
        if (success) it->active = false;
    }
    if (success) it->enabled = enabled;
    state_ = State::Running;
    return success;
}

bool ServerLauncher::StartAll()
{
    if (state_ == State::Running)
    {
        return true;
    }
    if (state_ != State::Stopped)
    {
        return false;
    }

    state_ = State::Starting;
    for (auto& item : services_)
    {
        if (!item.enabled || StartService(item)) continue;
        StopServices();
        return false;
    }

    state_ = State::Running;
    return true;
}

void ServerLauncher::StopAll() noexcept
{
    if (state_ == State::Running)
    {
        StopServices();
    }
}

void ServerLauncher::StopServices() noexcept
{
    state_ = State::Stopping;
    for (auto it = services_.rbegin(); it != services_.rend(); ++it)
    {
        if (!it->active) continue;
        RunCleanup(it->name, "stop", it->stop);
        it->active = false;
    }
    state_ = State::Stopped;
}

} // namespace server
