#ifndef _SERVICE_MANAGER_H_
#define _SERVICE_MANAGER_H_

#include <memory>
#include <mutex>
#include <unordered_map>
#include <vector>
#include "IService.h"

namespace service
{

class ServiceManager
{
public:
    ServiceManager() = default;
    ~ServiceManager() = default;

    ServiceManager(const ServiceManager&) = delete;
    ServiceManager& operator=(const ServiceManager&) = delete;


    bool Register(std::shared_ptr<IService> service);

    bool InitAll();
    bool StartAll();

    void StopAll();
    void ShutdownAll();

    std::shared_ptr<IService> Find(ServiceType type) const;
    std::vector<ServiceHealth> HealthSnapshot() const;

private:
    using ServicePtr = std::shared_ptr<IService>;
    using ServiceList = std::vector<ServicePtr>;
    static uint16_t MakeKey(ServiceType type);

    std::size_t Size() const;

    /**
     * 对当前服务列表创建快照。
     *
     * 后续调用 IService 接口时不持有 mutex_，避免死锁
     */
    ServiceList Snapshot() const;

private:
    mutable std::mutex mutex_;
    ServiceList services_;
    std::unordered_map<uint16_t, ServicePtr> service_map_;

};

}

#endif /* _SERVICE_MANAGER_H_ */