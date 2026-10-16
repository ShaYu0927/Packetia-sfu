#include "ControlApp.h"
#include "ClusterController.h"
#include "RuntimeConfig.h"
#include "AccountController.h"
#include "ApiServer.h"
#include "HttpServer.h"
#include "SqliteAccountStore.h"
#include "ScryptPasswordHasher.h"
#include "EventLoop.h"

namespace server::cluster
{
struct ControlApp::Impl
{
    EventLoop loop{1};
    std::shared_ptr<service::account::AccountService> accounts;
    std::shared_ptr<service::cluster::Coordinator> coordinator;
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
        accounts = std::make_shared<service::account::AccountService>(options,
            std::make_shared<service::account::SqliteAccountStore>(),
            std::make_shared<service::account::ScryptPasswordHasher>());
        coordinator = std::make_shared<service::cluster::Coordinator>(cluster_path);
        ClusterController controller(accounts, coordinator, NodeSecrets(), JwtIssuer(), JwtSecret());
        protocol::http::HttpServerOptions http;
        http.enabled = true;
        http.listen_ip = config.at("listen_ip").get<std::string>();
        http.listen_port = Port(config, "http_port");
        api::ApiRouter::Builder routes;
        api::AccountController(accounts).RegisterRoutes(routes);
        controller.RegisterPublic(routes);
        public_api = std::make_unique<api::ApiServer>(http, routes.Build(http.max_body_bytes),
            std::make_shared<protocol::http::HttpServer>(&loop));
        http.listen_ip = config.at("internal_listen_ip").get<std::string>();
        http.listen_port = Port(config, "internal_http_port");
        http.max_connections = 512;
        api::ApiRouter::Builder internal;
        controller.RegisterInternal(internal);
        internal_api = std::make_unique<api::ApiServer>(http, internal.Build(http.max_body_bytes),
            std::make_shared<protocol::http::HttpServer>(&loop), 2, 512);
    }
    void Stop() noexcept
    {
        if (public_api) public_api->Stop();
        if (internal_api) internal_api->Stop();
        if (accounts) accounts->Stop();
        loop.Stop();
    }
};
ControlApp::ControlApp(const std::string& path) : impl_(std::make_unique<Impl>(path)) {}
ControlApp::~ControlApp() { Stop(); }
bool ControlApp::Start()
{
    if (!impl_->accounts->Start().Succeeded() || !impl_->loop.Start() ||
        !impl_->internal_api->Start() || !impl_->public_api->Start())
    {
        Stop();
        return false;
    }
    return true;
}
void ControlApp::Stop() noexcept { impl_->Stop(); }
}
