#pragma once
#include "ApiRouter.h"
#include <memory>

namespace service { class MonitorService; }
namespace server::api
{
class MetricsController
{
public:
    explicit MetricsController(std::shared_ptr<service::MonitorService> monitor);
    void RegisterRoutes(ApiRouter::Builder& routes) const;
private:
    std::weak_ptr<service::MonitorService> monitor_;
};
}
