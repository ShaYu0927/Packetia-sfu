#include "MetricsController.h"
#include "MetricsJsonEncoder.h"
#include "MonitorService.h"
#include "ApiResponse.h"

namespace server::api
{
MetricsController::MetricsController(std::shared_ptr<service::MonitorService> monitor) : monitor_(monitor) {}
void MetricsController::RegisterRoutes(ApiRouter::Builder& routes) const
{
    const auto read = [weak = monitor_](const ApiRequest&) {
        auto monitor = weak.lock();
        auto snapshot = monitor ? monitor->Snapshot() : nullptr;
        if (!monitor || monitor->State() != service::ServiceState::Running || !snapshot)
            return ErrorResponse(503, "unavailable", "Monitoring unavailable.");
        return protocol::http::HttpResponse{200, EncodeMetricsJson(*snapshot, monitor->Health()),
            {{"content-type", "application/json; charset=utf-8"}, {"cache-control", "no-store"},
             {"x-content-type-options", "nosniff"}}};
    };
    routes.Get("/metrics", read);
    routes.Get("/api/v1/metrics", read);
}
}
