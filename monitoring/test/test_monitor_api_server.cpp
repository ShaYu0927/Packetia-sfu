#include "ApiServer.h"
#include "MetricsController.h"
#include "MonitorService.h"
#include "nlohmann/json.hpp"
#include <future>
#include <iostream>
#include <stdexcept>

#define CHECK(expr) do { if (!(expr)) throw std::runtime_error(#expr); } while (false)
class FakeTransport final : public protocol::http::IHttpServer
{
public:
    bool Start(const protocol::http::HttpServerOptions&, RequestHandler callback) override
    { handler = std::move(callback); return accept_start; }
    void Stop() noexcept override { handler = {}; }
    RequestHandler handler;
    bool accept_start = true;
};
protocol::http::HttpResponse Get(const protocol::http::IHttpServer::RequestHandler& handler)
{
    auto promise = std::make_shared<std::promise<protocol::http::HttpResponse>>();
    auto result = promise->get_future();
    handler({"GET", "/metrics", {}, {}}, [promise](auto response) { promise->set_value(std::move(response)); });
    CHECK(result.wait_for(std::chrono::seconds(3)) == std::future_status::ready);
    return result.get();
}
int main()
try
{
    monitoring::MetricsContext context;
    auto monitor = std::make_shared<service::MonitorService>(context.Registry());
    server::api::ApiRouter::Builder routes;
    server::api::MetricsController(monitor).RegisterRoutes(routes);
    auto transport = std::make_shared<FakeTransport>();
    protocol::http::HttpServerOptions options;
    options.enabled = true;
    const auto previous = WorkerService::pool_names();
    {
        server::api::ApiServer api(options, routes.Build(), transport, 1, 64);
        CHECK(api.Start()); // No account options, service, database or account library.
        CHECK(Get(transport->handler).status == 503);
        CHECK(monitor->Init() && monitor->Start());
        auto response = Get(transport->handler);
        CHECK(response.status == 200 && nlohmann::json::parse(response.body)["schema_version"] == 1);
        auto stale_handler = transport->handler;
        api.Stop();
        CHECK(Get(stale_handler).status == 503);
    }
    CHECK(WorkerService::pool_names() == previous);
    transport->accept_start = false;
    {
        server::api::ApiServer failed(options, routes.Build(), transport, 1, 64);
        CHECK(!failed.Start());
    }
    CHECK(WorkerService::pool_names() == previous);
    monitor->Shutdown();
    std::cout << "Monitoring API works without account dependencies and cleans up failed startup\n";
    return 0;
}
catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
