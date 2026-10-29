#ifndef _SERVICE_BASE_H_
#define _SERVICE_BASE_H_

#include "IService.h"
#include <atomic>
#include <mutex>

namespace service 
{
  
class ServiceBase : public IService
{
public:
    ServiceState State() const override;
    ServiceHealth Health() const override;

    bool Init() final;
    bool Start() final;
    void Stop() final;
    void Shutdown() final;

protected:
    virtual bool OnInit() = 0;
    virtual bool OnStart() = 0;
    virtual void OnStop() = 0;
    virtual void OnShutdown() = 0;

    void SetFailed(int error_code);

private:
    // 生命周期由管理线程串行调用；业务就绪检查使用 State()，不持有此锁。
    std::mutex lifecycle_mutex_;
    std::atomic<ServiceState> state_{ServiceState::Created};
    std::atomic<int> error_code_{0};
};


}


#endif /* _SERVICE_BASE_H_ */
