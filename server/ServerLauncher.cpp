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
void RunCleanup(const std::string& name, const char* operation,
    Callback&& callback) noexcept
{
    try
    {
        callback();
    }
    catch (const std::exception& error)
    {
        LogFailure(name, operation, error.what());
    }
    catch (...)
    {
        LogFailure(name, operation, "unknown exception");
    }
}
} // namespace

ServerLauncher::~ServerLauncher() noexcept
{
    StopAll();
}

void ServerLauncher::AddService(std::string name,
    std::shared_ptr<service::IService> instance)
{
    if (!instance)
    {
        throw std::invalid_argument("cannot register a null service");
    }

    AddCustomService(name,
        [instance] { return instance->Init() && instance->Start(); },
        [instance, name] {
            RunCleanup(name, "stop", [&] { instance->Stop(); });
            RunCleanup(name, "shutdown", [&] { instance->Shutdown(); });
        });
}

void ServerLauncher::AddCustomService(const std::string& name,
    std::function<bool()> start, std::function<void()> stop)
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

    services_.push_back({name, std::move(start), std::move(stop)});
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
    while (active_count_ < services_.size())
    {
        // Include this attempt in rollback even if it initializes only partly.
        const auto& item = services_[active_count_++];
        try
        {
            if (item.start())
            {
                continue;
            }
            LogFailure(item.name, "start", "returned false");
        }
        catch (const std::exception& error)
        {
            LogFailure(item.name, "start", error.what());
        }
        catch (...)
        {
            LogFailure(item.name, "start", "unknown exception");
        }

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
    while (active_count_ != 0)
    {
        const auto& item = services_[--active_count_];
        RunCleanup(item.name, "stop", item.stop);
    }
    state_ = State::Stopped;
}

} // namespace server
