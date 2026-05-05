#ifndef _SERVER_APP_H_
#define _SERVER_APP_H_

#ifndef _SERVER_LAUNCHER_H_
#define _SERVER_LAUNCHER_H_

#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace server
{

class ServerLauncher
{
public:
    ServerLauncher() = default;
    ~ServerLauncher()
    {
        StopAll();
    }

    ServerLauncher(const ServerLauncher&) = delete;
    ServerLauncher& operator=(const ServerLauncher&) = delete;

public:
    /**
     * @brief 添加一个 ip + port 类型的服务。
     *
     * 要求 ServiceT 支持：
     * bool Start(const std::string& ip, uint16_t port);
     * void Stop();
     *
     */
    template <typename ServiceT, typename... Args>
    std::shared_ptr<ServiceT> AddIpPortService(const std::string& name, const std::string& ip, uint16_t port, Args&&... args)
    {
        auto service = std::make_shared<ServiceT>(std::forward<Args>(args)...);

        ServiceItem item;
        item.name = name;

        item.start = [service, ip, port]() -> bool {
            return service->Start(ip, port);
        };

        item.stop = [service]() {
            service->Stop();
        };

        services_.push_back(std::move(item));
        return service;
    }

    /**
     * @brief 添加一个普通服务。
     *
     * 要求 ServiceT 支持：
     * bool Start();
     * void Stop();
     */
    template <typename ServiceT, typename... Args>
    std::shared_ptr<ServiceT> AddService(const std::string& name, Args&&... args)
    {
        auto service = std::make_shared<ServiceT>(std::forward<Args>(args)...);

        ServiceItem item;
        item.name = name;

        item.start = [service]() -> bool 
        {
            return service->Start();
        };

        item.stop = [service]() 
        {
            service->Stop();
        };

        services_.push_back(std::move(item));
        return service;
    }

    /**
     * @brief 添加自定义启动逻辑的服务。
     *
     * 适合某些接口不统一的服务。
     */
    void AddCustomService(const std::string& name, std::function<bool()> start, std::function<void()> stop)
    {
        ServiceItem item;
        item.name = name;
        item.start = std::move(start);
        item.stop = std::move(stop);

        services_.push_back(std::move(item));
    }

    bool StartAll()
    {
        for (auto& service : services_)
        {
            if (service.started)
            {
                continue;
            }

            if (!service.start || !service.start())
            {
                StopStartedServices();
                return false;
            }

            service.started = true;

        }

        return true;
    }

    void StopAll()
    {
        for (auto it = services_.rbegin(); it != services_.rend(); ++it)
        {
            if (!it->started)
            {
                continue;
            }

            if (it->stop)
            {
                it->stop();
            }

            it->started = false;
        }
    }

private:
    void StopStartedServices()
    {
        for (auto it = services_.rbegin(); it != services_.rend(); ++it)
        {
            if (!it->started)
            {
                continue;
            }

            if (it->stop)
            {
                it->stop();
            }

            it->started = false;
        }
    }

private:
    struct ServiceItem
    {
        std::string name;
        std::function<bool()> start;
        std::function<void()> stop;
        bool started{false};
    };

    std::vector<ServiceItem> services_;
};

} // namespace server

#endif



#endif /* _SERVER_APP_H_ */