#include "ServiceManager.h"

namespace service 
{
bool ServiceManager::Register(std::shared_ptr<IService> service)
{
    if(!service)
    {
        return false;
    }

    const ServiceType type = service->Type();
    if (type == ServiceType::Unknown)
    {
        return false;
    }

    const uint16_t key = MakeKey(type);
    std::lock_guard<std::mutex> lock(mutex_);

    if (service_map_.find(key) != service_map_.end())
    {
        return false;
    }

    services_.push_back(service);

    try
    {
        service_map_.emplace(key, std::move(service));
    }
    catch (...)
    { 
        services_.pop_back();
        throw;
    }

    return true;
}

bool ServiceManager::InitAll()
{
    const ServiceList services = Snapshot();

    ServiceList initialized_services;
    initialized_services.reserve(services.size());

    for (const auto& service : services)
    {
        if (!service)
        {
            continue;
        }

        const ServiceState state = service->State();

        if (state == ServiceState::Initialized || state == ServiceState::Starting || state == ServiceState::Running)
        {
            continue; 
        }

        if (state == ServiceState::Stopping)
        {
            for (auto it = initialized_services.rbegin(); it != initialized_services.rend(); ++it)
            {
                (*it)->Shutdown();
            }
            return false;
        }

        bool success = false;
        try
        {
            success = service->Init();
        }
        catch (...)
        {
            success = false;
        }

        if(!success)
        {
            for (auto it = initialized_services.rbegin(); it != initialized_services.rend(); ++it)
            {
                try
                {
                    (*it)->Shutdown();
                }
                catch (...)
                {
                
                }
            }
            return false;
        }
        initialized_services.push_back(service);
    }
    return true;
}

bool ServiceManager::StartAll()
{
    const ServiceList services = Snapshot();
    ServiceList started_services;
    started_services.reserve(services.size());

    for (const auto& service : services)
    {
        if (!service)
        {
            continue;
        }

        const ServiceState state = service->State();
        if(state == ServiceState::Running)
        {
            continue;
        }

        if (state != ServiceState::Initialized && state != ServiceState::Stopped)
        {
            for (auto it = started_services.rbegin(); it != started_services.rend(); ++it)
            {
                try
                {
                    (*it)->Stop();
                }
                catch (...)
                {

                }
            }
            return false;
        }

        bool success = false;
        try
        {
            success = service->Start();
        }
        catch (...)
        {
            success = false;
        }


        if (!success)
        {
            for (auto it = started_services.rbegin(); it != started_services.rend(); ++it)
            {
                try
                {
                    (*it)->Stop();
                }
                catch (...)
                {
                    
                }
            }
            return false;
        }
        started_services.push_back(service);
    }
    return true;
}

void ServiceManager::StopAll()
{
    const ServiceList services = Snapshot();
    for (auto it = services.rbegin(); it != services.rend(); ++it)
    {
        const auto& service = *it;
        if (!service)
        {
            continue;
        }

        const ServiceState state = service->State();
        if (state != ServiceState::Running && state != ServiceState::Starting)
        {
            continue;
        }

        try
        {
            service->Stop();
        }
        catch (...)
        {
            
        }
    }
}

void ServiceManager::ShutdownAll()
{
    const ServiceList services = Snapshot();

    for (auto it = services.rbegin(); it != services.rend(); ++it)
    {
        const auto& service = *it;
        if (!service)
        {
            continue;
        }

        ServiceState state = service->State();

        if (state == ServiceState::Running || state == ServiceState::Starting)
        {
            try
            {
                service->Stop();
            }
            catch (...)
            {
                
            }

            state = service->State();
        }

        if (state == ServiceState::Created)
        {
            continue;
        }

        try
        {
            service->Shutdown();
        }
        catch (...)
        {
            
        }
    }
}

std::shared_ptr<IService> ServiceManager::Find(ServiceType type) const
{
    if (type == ServiceType::Unknown)
    {
        return nullptr;
    }

    const uint16_t key = MakeKey(type);

    std::lock_guard<std::mutex> lock(mutex_);

    const auto it = service_map_.find(key);
    if (it == service_map_.end())
    {
        return nullptr;
    }

    return it->second;
}


std::vector<ServiceHealth> ServiceManager::HealthSnapshot() const
{
    const ServiceList services = Snapshot();

    std::vector<ServiceHealth> result;
    result.reserve(services.size());

    for (const auto& service : services)
    {
        if (!service)
        {
            continue;
        }

        try
        {
            result.push_back(service->Health());
        }
        catch (...)
        {
            ServiceHealth health;
            health.healthy = false;
            health.type = service->Type();
            health.state = service->State();
            health.error_code = -1;

            result.push_back(health);
        }
    }

    return result;
}

std::size_t ServiceManager::Size() const
{
    std::lock_guard<std::mutex> lock(mutex_);
    return services_.size();
}

ServiceManager::ServiceList ServiceManager::Snapshot() const
{
    std::lock_guard<std::mutex> lock(mutex_);
    return services_;
}
}