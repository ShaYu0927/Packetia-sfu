#include "SfuApp.h"
#include "NodeClient.h"
#include "RuntimeConfig.h"
#include "WebRtcService.h"
#include "EventLoop.h"
#include "EndpointBase.h"
#include "WorkerRegistry.h"
#include "MetricsController.h"
#include "ApiServer.h"
#include "HttpServer.h"
#include "MonitorService.h"

namespace server::cluster
{
struct SfuApp::Impl
{
    monitoring::MetricsContext metrics_context{false};
    std::shared_ptr<service::MonitorService> monitor = std::make_shared<service::MonitorService>(metrics_context.Registry());
    EventLoop loop{1};
    WorkerRegistry workers;
    std::shared_ptr<WebRtcService> rtc;
    std::shared_ptr<service::cluster::NodeLease> lease;
    std::unique_ptr<NodeClient> client;
    std::unique_ptr<api::ApiServer> metrics_api;
    explicit Impl(const std::string& path)
    {
        const auto json = ReadConfig(path, {"node_id", "control_url", "signaling_url", "max_rooms",
            "listen_ip", "public_ip", "websocket_port", "udp_port", "max_participants",
            "metrics_http_port", "metrics_listen_ip"});
        if (json.contains("metrics_http_port"))
        {
            protocol::http::HttpServerOptions http;
            http.enabled = true;
            http.listen_ip = json.value("metrics_listen_ip", std::string("127.0.0.1"));
            http.listen_port = Port(json, "metrics_http_port");
            if (!http.IsValid()) throw std::runtime_error("invalid metrics HTTP config");
            api::ApiRouter::Builder routes;
            api::MetricsController(monitor).RegisterRoutes(routes);
            metrics_api = std::make_unique<api::ApiServer>(http, routes.Build(http.max_body_bytes),
                std::make_shared<protocol::http::HttpServer>(&loop), 1, 64);
        }
        service::cluster::Node node;
        node.id = json.at("node_id").get<std::string>();
        node.instance = service::cluster::Coordinator::RandomId();
        node.signaling_url = json.at("signaling_url").get<std::string>();
        node.max_rooms = Bounded(json, "max_rooms", 1, 10000);
        if (!service::cluster::Coordinator::ValidId(node.id) ||
            !service::cluster::Coordinator::ValidSignalingUrl(node.signaling_url)) throw std::runtime_error("invalid node config");
        config::AppConfig config;
        config.listen_ip = json.at("listen_ip").get<std::string>();
        config.webrtc.enabled = true;
        config.webrtc.public_ip = json.at("public_ip").get<std::string>();
        config.webrtc.max_sessions = Bounded(json, "max_participants", 1, 65536);
        config.webrtc.api_keys.emplace(JwtIssuer(), JwtSecret());
        config.websocket_port = Port(json, "websocket_port");
        config.udp_port = Port(json, "udp_port");
        config.recording_enabled = config.ai_enabled = config.conference_mix_enabled = false;
        config.Validate();
        lease = std::make_shared<service::cluster::NodeLease>(node.id, node.instance);
        rtc = std::make_shared<WebRtcService>(&loop, std::move(config), nullptr,
            [lease = lease](const service::auth::AuthContext& context) { return lease->Allows(context); });
        client = std::make_unique<NodeClient>(node, json.at("control_url").get<std::string>(),
            Env("PACKETIA_NODE_SECRET", 32), lease, [rtc = rtc] { return rtc->ParticipantCount(); });
        workers.Add({POOL_MEDIA, 2, 4096, ShardedWorkerPool::DropPolicy::DropHead},
            std::make_shared<utils::EndpointJobHandler>(&utils::EndpointManager::Instance()));
    }
    void Stop() noexcept
    {
        if (metrics_api) metrics_api->Stop();
        if (client) client->Stop();
        if (rtc) rtc->Stop();
        workers.Stop();
        loop.Stop();
        monitor->Stop();
        monitor->Shutdown();
        metrics_context.Deactivate();
    }
};
SfuApp::SfuApp(const std::string& path) : impl_(std::make_unique<Impl>(path)) {}
SfuApp::~SfuApp() { Stop(); }
bool SfuApp::Start()
{
    if (!impl_->metrics_context.Activate() || !impl_->monitor->Init() || !impl_->monitor->Start() || !impl_->loop.Start() ||
        impl_->workers.Start() != 0 || !impl_->rtc->Start() || !impl_->client->Start() ||
        (impl_->metrics_api && !impl_->metrics_api->Start()))
    {
        Stop();
        return false;
    }
    return true;
}
void SfuApp::Stop() noexcept { impl_->Stop(); }
}
