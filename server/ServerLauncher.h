#ifndef _SERVER_LAUNCHER_H_
#define _SERVER_LAUNCHER_H_

#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace service
{
class IService;
}

namespace server
{

// Lifecycle calls are serialized by the owner. Registration is allowed only
// before startup or after all services have stopped, never from a callback.
class ServerLauncher
{
public:
    ServerLauncher() = default;
    ~ServerLauncher() noexcept;

    ServerLauncher(const ServerLauncher&) = delete;
    ServerLauncher& operator=(const ServerLauncher&) = delete;

    // ServiceT implements bool Start(const std::string&, uint16_t) and Stop().
    template <typename ServiceT, typename... Args>
    std::shared_ptr<ServiceT> AddIpPortService(const std::string& name,
        const std::string& ip, uint16_t port, Args&&... args)
    {
        auto instance = std::make_shared<ServiceT>(std::forward<Args>(args)...);
        AddCustomService(name,
            [instance, ip, port] { return instance->Start(ip, port); },
            [instance] { instance->Stop(); });
        return instance;
    }

    // Owns the service through Init/Start and Stop/Shutdown, including rollback.
    void AddService(std::string name, std::shared_ptr<service::IService> instance);

    // Names must be nonempty and unique, and both callbacks must be supplied.
    // Stop must tolerate a partially completed or failed start attempt.
    void AddCustomService(const std::string& name,
        std::function<bool()> start, std::function<void()> stop);

    // Starts in registration order; failure rolls back in reverse order.
    // Repeated calls while running succeed without starting services again.
    bool StartAll();
    void StopAll() noexcept;

private:
    enum class State { Stopped, Starting, Running, Stopping };

    struct ServiceItem
    {
        std::string name;
        std::function<bool()> start;
        std::function<void()> stop;
    };

    void StopServices() noexcept;

    std::vector<ServiceItem> services_;
    std::size_t active_count_{0};
    State state_{State::Stopped};
};

} // namespace server

#endif // _SERVER_LAUNCHER_H_
