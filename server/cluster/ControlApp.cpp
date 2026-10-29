#include "ControlApp.h"
#include "NodeController.h"
#include "RoomController.h"
#include "NodeCommandClient.h"
#include "MeetingModule.h"
#include "ServiceManager.h"
#include "RuntimeConfig.h"
#include "AccountController.h"
#include "ApiResponse.h"
#include "ApiServer.h"
#include "HttpServer.h"
#include "SqliteAccountStore.h"
#include "ScryptPasswordHasher.h"
#include "EventLoop.h"
#include "MetricsController.h"
#include "MonitorService.h"

namespace server::cluster
{
struct ControlApp::Impl
{
    monitoring::MetricsContext metrics_context{false};
    std::shared_ptr<service::MonitorService> monitor = std::make_shared<service::MonitorService>(metrics_context.Registry());
    EventLoop loop{1};
    std::shared_ptr<service::account::AccountService> accounts;
    std::shared_ptr<service::meeting::MeetingModule> meetings;
    service::ServiceManager services;
    std::unique_ptr<api::ApiServer> public_api, internal_api;
    explicit Impl(const std::string& path)
    {
        const auto config = ReadConfig(path, {"listen_ip", "http_port", "internal_listen_ip", "internal_http_port",
            "account_database", "cluster_database", "registration_enabled"});
        service::account::AccountOptions options;
        options.enabled = true;
        options.registration_enabled = config.value("registration_enabled", false);
        options.database_path = config.at("account_database").get<std::string>();
        if (!options.IsValid()) throw std::runtime_error("invalid account config");
        const auto cluster_path = config.at("cluster_database").get<std::string>();
        if (cluster_path.empty()) throw std::runtime_error("invalid cluster database");
        accounts = std::make_shared<service::account::AccountService>(options,std::make_shared<service::account::SqliteAccountStore>(), std::make_shared<service::account::ScryptPasswordHasher>());
        meetings = std::make_shared<service::meeting::MeetingModule>(service::meeting::MeetingModuleOptions{cluster_path, JwtIssuer(), JwtSecret()}, MakeNodeCommandSender(NodeSecrets()));
        if (!services.Register(monitor) || !services.Register(meetings))
            throw std::runtime_error("cannot register control services");
        meeting::RoomController rooms(accounts, meetings);
        NodeController nodes(meetings, NodeSecrets());
        protocol::http::HttpServerOptions http;
        http.enabled = true;
        http.listen_ip = config.at("listen_ip").get<std::string>();
        http.listen_port = Port(config, "http_port");
        http.max_body_bytes = 256 * 1024;
        api::ApiRouter::Builder routes;
        api::AccountController(accounts).RegisterRoutes(routes);
        api::MetricsController(monitor).RegisterRoutes(routes);
        routes.Get("/healthz", [module = meetings](const auto&) {
            const auto health = module->Health();
            return api::JsonResponse(health.healthy ? 200 : 503,
                {{"status", health.healthy ? "ok" : "unavailable"}, {"role", "control"},
                 {"meeting_state", static_cast<uint8_t>(health.state)}, {"meeting_error", health.error_code}});
        });
        rooms.RegisterRoutes(routes);
        public_api = std::make_unique<api::ApiServer>(http, routes.Build(http.max_body_bytes),
            std::make_shared<protocol::http::HttpServer>(&loop));
        http.listen_ip = config.at("internal_listen_ip").get<std::string>();
        http.listen_port = Port(config, "internal_http_port");
        http.max_connections = 512;
        api::ApiRouter::Builder internal;
        nodes.RegisterRoutes(internal);
        internal_api = std::make_unique<api::ApiServer>(http, internal.Build(http.max_body_bytes),
            std::make_shared<protocol::http::HttpServer>(&loop), 2, 512);
    }
    void Stop() noexcept
    {
        services.StopAll();
        if (public_api) public_api->Stop();
        if (internal_api) internal_api->Stop();
        if (accounts) accounts->Stop();
        loop.Stop();
        services.ShutdownAll();
        metrics_context.Deactivate();
    }
};
ControlApp::ControlApp(const std::string& path) : impl_(std::make_unique<Impl>(path)) {}
ControlApp::~ControlApp() { Stop(); }
bool ControlApp::Start()
{
    if (!impl_->metrics_context.Activate() || !impl_->services.InitAll() ||
        !impl_->accounts->Start().Succeeded() || !impl_->loop.Start() ||
        !impl_->services.StartAll() ||
        !impl_->internal_api->Start() || !impl_->public_api->Start())
    {
        Stop();
        return false;
    }
    return true;
}
void ControlApp::Stop() noexcept { impl_->Stop(); }
}
