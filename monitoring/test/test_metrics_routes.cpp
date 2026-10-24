#include "MetricsController.h"
#include "MonitorService.h"
#include "nlohmann/json.hpp"
#include <iostream>
#include <stdexcept>

#define CHECK(expr) do { if (!(expr)) throw std::runtime_error(#expr); } while (false)
int main()
try
{
    monitoring::MetricsContext context;
    auto worker = monitoring::RegisterWorker("http_test", 0);
    worker->Queue(2, 2048, monitoring::NowNs() - 1000000);
    auto monitor = std::make_shared<service::MonitorService>(context.Registry(), std::chrono::seconds(10));
    server::api::ApiRouter::Builder routes;
    server::api::MetricsController(monitor).RegisterRoutes(routes);
    const auto router = routes.Build();
    CHECK(router.Dispatch({"GET", "/metrics", {}, {}}).status == 503);
    CHECK(monitor->Init() && monitor->Start());
    for (const auto* path : {"/metrics", "/api/v1/metrics"})
    {
        const auto response = router.Dispatch({"GET", path, {}, {}});
        CHECK(response.status == 200 && response.headers.at("cache-control") == "no-store");
        auto json = nlohmann::json::parse(response.body);
        CHECK(json["workers"][0]["pool"] == "http_test" && json["workers"][0]["queue_bytes"] == 2048);
        CHECK(json["monitor"]["healthy"] == true && json["monitor"].contains("snapshot_age_ms"));
        CHECK(router.Dispatch({"POST", path, {}, {}}).status == 405);
        CHECK(router.Dispatch({"GET", path, "unexpected", {}}).status == 400);
    }
    auto first = monitor->Snapshot();
    worker->Queue(4, 4096, monitoring::NowNs());
    CHECK(monitor->Snapshot() == first);
    CHECK(nlohmann::json::parse(router.Dispatch({"GET", "/metrics", {}, {}}).body)["workers"][0]["queue_bytes"] == 2048);
    monitor->Stop();
    CHECK(router.Dispatch({"GET", "/metrics", {}, {}}).status == 503);
    CHECK(monitor->Start());
    CHECK(monitor->Snapshot()->runtime.workers[0].queue_bytes == 4096);
    monitor.reset();
    CHECK(router.Dispatch({"GET", "/metrics", {}, {}}).status == 503);
    std::cout << "Metrics routes: cached JSON, service lifecycle and expired ownership passed\n";
    return 0;
}
catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
