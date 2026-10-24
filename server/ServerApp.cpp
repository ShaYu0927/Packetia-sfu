#include "ServerApp.h"
#include "ServiceNames.h"
#include "WorkerSetup.h"
#include "ApiServer.h"
#include "AccountController.h"
#include "MetricsController.h"
#include "MonitorService.h"
#include "HttpServer.h"
#include "AccountService.h"
#include "ScryptPasswordHasher.h"
#include "SqliteAccountStore.h"

#include "EventLoop.h"
#include "RtspServer.h"
#include "RtspMediaSession.h"
#include "SipServer.h"
#include "rtmp_server.h"
#include "UdpServer.h"
#include "UdpSession.h"
#include "TurnServer.h"
#include "StringUtil.h"
#include "AIService.h"
#include "UnavailableModelProvider.h"
#include "RecordingService.h"
#include "TranscodeService.h"
#include "EncodedFrameRouter.h"
#include "logger.h"

#ifdef PACKETIA_WITH_LIBWEBSOCKETS
#include "WsServer.h"
#include "WebRtcService.h"
#endif

#include <stdexcept>
#include <cstdlib>
#include <utility>

namespace server
{
ServerApp::ServerApp(ServerConfig config, std::shared_ptr<service::IRecordingEventSink> recording_events)
    : monitor_service_(std::make_shared<service::MonitorService>(metrics_context_.Registry())),
      config_(std::move(config)),
      settings_(std::make_shared<config::ConfigStore>(config_)),
      recording_events_(std::move(recording_events)),
      event_loop_(std::make_shared<EventLoop>(config_.io_threads)),
      frame_router_(std::make_shared<media::EncodedFrameRouter>()),
      mix_output_router_(std::make_shared<media::EncodedFrameRouter>()),
      transcode_output_router_(std::make_shared<media::EncodedFrameRouter>()),
      transcode_service_(std::make_shared<service::transcode::TranscodeService>(frame_router_, transcode_output_router_)),
      mix_service_(std::make_shared<service::mix::ConferenceMixService>(frame_router_, mix_output_router_, settings_))
{
    ConfigureServices();
}

ServerApp::~ServerApp()
{
    Stop();
}

bool ServerApp::Start()
{
    std::lock_guard<std::mutex> lock(lifecycle_mutex_);
    if (!metrics_context_.Activate()) return false;
    const bool started = launcher_.StartAll();
    if (!started) metrics_context_.Deactivate();
    return started;
}

void ServerApp::Run()
{
    event_loop_->Loop();
}

void ServerApp::Stop() noexcept
{
    std::lock_guard<std::mutex> lock(lifecycle_mutex_);
    launcher_.StopAll();
    metrics_context_.Deactivate();
}

ThreadRuntimeStatus ServerApp::ThreadStatus() const
{
    return {utils::ThreadManager::Instance().Snapshot(), WorkerService::status(), event_loop_->Status()};
}

void ServerApp::ConfigureServices()
{
    // Monitor starts before producers and stops after they have drained.
    launcher_.AddService(SERVICE_MONITOR, monitor_service_);
    // Reverse shutdown keeps I/O and frame consumers alive while workers
    // drain, including cleanup jobs submitted when network servers stop.
    launcher_.AddCustomService(SERVICE_LOOP,
        [loop = event_loop_] { return loop->Start(); },
        [loop = event_loop_] { loop->Stop(); });

    launcher_.AddCustomService(SERVICE_PUBLISHER,
        [router = frame_router_] {
            MediaSessionManager::Instance().SetFramePublisher(router);
            return true;
        },
        [] { MediaSessionManager::Instance().SetFramePublisher(nullptr); });

    // Account resources outlive API callbacks and their business workers.
    AddAccountServices();
    AddMediaServices();
    AddWorkerPools(launcher_, config_.workers);
    AddNetworkServices();
    // Keep API registration outside AddNetworkServices: its WebRTC path returns early.
    AddApiService();
}

void ServerApp::AddAccountServices()
{
    if (!config_.account.enabled) return;
    auto store = std::make_shared<service::account::SqliteAccountStore>();
    auto passwords = std::make_shared<service::account::ScryptPasswordHasher>();
    account_service_ = std::make_shared<service::account::AccountService>(config_.account, store, passwords);
    const auto accounts = account_service_;
    launcher_.AddCustomService(SERVICE_ACCOUNT,
        [accounts] {
            const auto result = accounts->Start();
            if (!result.Succeeded())
                LOG_ERROR("Account service unavailable: error=", static_cast<int>(result.error),
                          "; check the account database path and password provider");
            return result.Succeeded();
        },
        [accounts] { accounts->Stop(); });
}

void ServerApp::AddApiService()
{
    if (!config_.http.enabled) return;
    if (!account_service_)
        throw std::invalid_argument("HTTP account API requires the account service");
    auto transport = std::make_shared<protocol::http::HttpServer>(event_loop_.get());
    api::ApiRouter::Builder routes;
    api::AccountController(account_service_).RegisterRoutes(routes);
    api::MetricsController(monitor_service_).RegisterRoutes(routes);
    api_server_ = std::make_shared<api::ApiServer>(config_.http, routes.Build(config_.http.max_body_bytes), transport,
        config_.account.worker_count, config_.account.max_pending_requests);
    const auto api = api_server_;
    launcher_.AddCustomService(SERVICE_API,
        [api] {
            if (api->Start()) return true;
            LOG_ERROR("Account HTTP API failed to start; check HTTP backend, I/O owner and listen address");
            return false;
        },
        [api] { api->Stop(); });
}

void ServerApp::AddMediaServices()
{
    // Register disabled services too, so they can be enabled without restarting.
    recording_service_ = std::make_shared<service::RecordingService>(frame_router_, config_.recording, recording_events_, settings_);
    launcher_.AddService(SERVICE_RECORD, recording_service_, config_.recording_enabled);
    launcher_.AddService(SERVICE_AI,
        std::make_shared<service::ai::AIService>(
            std::make_shared<service::ai::UnavailableModelProvider>(), frame_router_, nullptr, 128, settings_), config_.ai_enabled);
    launcher_.AddService(SERVICE_CONFERENCE_MIX, mix_service_, config_.conference_mix_enabled);
    // No transcode workers are allocated until a task is explicitly started.
    launcher_.AddService(SERVICE_TRANSCODE, transcode_service_);
}

bool ServerApp::SetServiceEnabled(service::ServiceType type, bool enabled)
{
    std::lock_guard<std::mutex> lock(lifecycle_mutex_);
    const char* name = nullptr;
    config::Feature feature;
    switch (type) 
    {
    case service::ServiceType::Transcode:
        return launcher_.SetEnabled(SERVICE_TRANSCODE, enabled);
    case service::ServiceType::Record: name = SERVICE_RECORD; feature = config::Feature::Recording; break;
    case service::ServiceType::Ai: name = SERVICE_AI; feature = config::Feature::AI; break;
    case service::ServiceType::ConferenceMix:
        name = SERVICE_CONFERENCE_MIX; feature = config::Feature::ConferenceMix; break;
    default: return false;
    }
    if (!launcher_.SetEnabled(name, enabled)) return false;
    settings_->SetServiceEnabled(feature, enabled);
    return true;
}

bool ServerApp::ServiceEnabled(service::ServiceType type) const
{
    std::lock_guard<std::mutex> lock(lifecycle_mutex_);
    const auto settings = settings_->Snapshot();
    switch (type) 
    {
    case service::ServiceType::Transcode: return launcher_.IsEnabled(SERVICE_TRANSCODE);
    case service::ServiceType::Record: return settings->recording_enabled;
    case service::ServiceType::Ai: return settings->ai_enabled;
    case service::ServiceType::ConferenceMix: return settings->conference_mix_enabled;
    default: return false;
    }
}

void ServerApp::RefreshStreamPolicies()
{
    if (recording_service_) recording_service_->RefreshStreamPolicies();
    mix_service_->RefreshStreamPolicies();
}

bool ServerApp::StartRecording(const config::StreamKey& stream)
{
    std::lock_guard<std::mutex> lock(lifecycle_mutex_);
    return recording_service_->StartRecording(stream);
}

bool ServerApp::StopRecording(const config::StreamKey& stream)
{
    std::lock_guard<std::mutex> lock(lifecycle_mutex_);
    return recording_service_->StopRecording(stream);
}

std::vector<service::SegmentInfo> ServerApp::QueryRecordingSegments(const service::SegmentQuery& query) const
{
    return recording_service_->QuerySegments(query);
}

std::vector<service::RecordedStream> ServerApp::ListRecordedStreams(const std::string& session_id) const
{
    return recording_service_->ListRecordedStreams(session_id);
}

void ServerApp::SetStreamConfig(config::StreamKey stream, config::StreamOverrides overrides)
{
    std::lock_guard<std::mutex> lock(lifecycle_mutex_);
    settings_->SetStreamConfig(std::move(stream), overrides);
    RefreshStreamPolicies();
}

void ServerApp::RemoveStreamConfig(const config::StreamKey& stream)
{
    std::lock_guard<std::mutex> lock(lifecycle_mutex_);
    settings_->RemoveStreamConfig(stream);
    RefreshStreamPolicies();
}

void ServerApp::SetStreamDefaults(config::StreamFeatures defaults)
{
    std::lock_guard<std::mutex> lock(lifecycle_mutex_);
    settings_->SetStreamDefaults(defaults);
    RefreshStreamPolicies();
}

config::StreamFeatures ServerApp::EffectiveStreamConfig(const config::StreamKey& stream) const
{
    return settings_->Resolve(stream);
}

std::shared_ptr<const config::AppConfig> ServerApp::ConfigSnapshot() const
{
    return settings_->Snapshot();
}

bool ServerApp::StartConference(service::mix::MixConfig config,
                                std::unique_ptr<service::mix::IMixBackend> backend)
{
    std::lock_guard<std::mutex> lock(lifecycle_mutex_);
    return mix_service_->StartConference(std::move(config), std::move(backend));
}

bool ServerApp::StopConference(const std::string& room_id)
{
    std::lock_guard<std::mutex> lock(lifecycle_mutex_);
    return mix_service_->StopConference(room_id);
}

bool ServerApp::StartTranscode(service::transcode::TranscodeTaskConfig config,
                               std::function<void()> request_keyframe)
{
    std::lock_guard<std::mutex> lock(lifecycle_mutex_);
    return transcode_service_->StartTask(std::move(config), std::move(request_keyframe));
}

bool ServerApp::StopTranscode(const std::string& task_id, service::transcode::TranscodeTaskStatus* final_status)
{
    std::lock_guard<std::mutex> lock(lifecycle_mutex_);
    return transcode_service_->StopTask(task_id, final_status);
}

std::optional<service::transcode::TranscodeTaskStatus> ServerApp::TranscodeStatus(const std::string& task_id) const
{
    return transcode_service_->TaskStatus(task_id);
}

std::string ServerApp::TranscodeLastError() const
{
    return transcode_service_->LastError();
}

bool ServerApp::RenegotiateWebRtc(uint64_t session_id, const std::vector<sdp::SdpMedia>& medias, std::string& error)
{
    std::lock_guard<std::mutex> lock(lifecycle_mutex_);
#ifdef PACKETIA_WITH_LIBWEBSOCKETS
    if (webrtc_service_) return webrtc_service_->Renegotiate(session_id, medias, error);
#endif
    error = "WebRTC service is not enabled";
    return false;
}

void ServerApp::AddNetworkServices()
{
    AddTurnService();
    const auto& ip = config_.listen_ip;
    auto* loop = event_loop_.get();
    launcher_.AddIpPortService<RtspServer>(SERVICE_RTSP, ip, config_.rtsp_port, loop);
    launcher_.AddIpPortService<SipServer>(SERVICE_SIP, ip, config_.sip_port, loop);
    launcher_.AddIpPortService<protocol::rtmp::RtmpServer>(SERVICE_RTMP, ip, config_.rtmp_port, loop);

    if (config_.webrtc.enabled)
    {
#ifdef PACKETIA_WITH_LIBWEBSOCKETS
        webrtc_service_ = std::make_shared<WebRtcService>(loop, config_, frame_router_);
        const auto rtc = webrtc_service_;
        launcher_.AddCustomService(SERVICE_WEBRTC,
            [rtc] { return rtc->Start(); }, [rtc] { rtc->Stop(); });
        return;
#else
        throw std::invalid_argument("WebRTC requires PACKETIA_WITH_LIBWEBSOCKETS");
#endif
    }

    auto udp = launcher_.AddIpPortService<network::UdpServer>(SERVICE_UDP, ip, config_.udp_port, loop);
    udp->SetHandler(std::make_shared<network::UdpMuxHandler>(udp.get()));

#ifdef PACKETIA_WITH_LIBWEBSOCKETS
    auto ws = launcher_.AddIpPortService<network::websocket::WsServer>(
        SERVICE_WS, ip, config_.websocket_port, loop);
    ws->SetOnOpen([](const network::websocket::WsConnectionInfo& info) {
        LOG_DEBUG("ws open, connId=", info.connId);
    });
    ws->SetOnMessage([](const std::string& conn_id, const std::string& message) {
        LOG_DEBUG("ws message, connId=", conn_id, ", bytes=", message.size());
        return std::string(R"({"code":0,"msg":"ok"})");
    });
    ws->SetOnClose([](const std::string& conn_id) {
        LOG_DEBUG("ws close, connId=", conn_id);
    });
#endif
}

void ServerApp::AddTurnService()
{
    if (!config_.turn.enabled) return;
    launcher_.AddCustomService(SERVICE_TURN,
        [this] {
            const char* user = std::getenv("PACKETIA_TURN_USER");
            const char* secret = std::getenv("PACKETIA_TURN_PASSWORD");
            if (!user || !secret)
            {
                LOG_ERROR("TURN requires PACKETIA_TURN_USER and PACKETIA_TURN_PASSWORD");
                return false;
            }
            const std::string username(user), password(secret);
            if (username.empty() || username.size() > PACKETIA_STUN_USERNAME_MAX_BYTES ||
                password.empty() || password.size() > PACKETIA_STUN_TEXT_MAX_BYTES ||
                !utils::IsPrintableAscii(username) || !utils::IsPrintableAscii(password))
            {
                LOG_ERROR("TURN requires nonempty printable ASCII credentials within protocol limits");
                return false;
            }
            protocol::TurnServerOptions options;
            const auto& config = config_.turn;
            options.listen_ip = config.listen_ip;
            options.listen_port = config.listen_port;
            options.dual_stack = config.dual_stack;
            options.relay_bind_ip = config.relay_bind_ip;
            options.advertised_ip = config.advertised_ip;
            options.relay_bind_ip_v6 = config.relay_bind_ip_v6;
            options.advertised_ip_v6 = config.advertised_ip_v6;
            options.realm = config.realm;
            options.relay_port_min = config.relay_port_min;
            options.relay_port_max = config.relay_port_max;
            options.max_sessions = config.max_sessions;
            options.max_allocations = config.max_allocations;
            options.max_allocations_per_user = config.max_allocations_per_user;
            options.max_permissions = config.max_permissions;
            options.max_channels = config.max_channels;
            options.unauthorized_ratelimit = config.unauthorized_ratelimit;
            options.unauthorized_ratelimit_rps = config.unauthorized_ratelimit_rps;
            if (config.local_test)
            {
                options.allow_peer = [](const network::SocketAddr& peer) {
                    if (peer.IsV6())
                        return IN6_IS_ADDR_LOOPBACK(&reinterpret_cast<const sockaddr_in6*>(&peer.ss)->sin6_addr);
                    const auto bytes = peer.IPv4Bytes();
                    return bytes.size() == PACKETIA_STUN_IPV4_ADDRESS_BYTES &&
                        static_cast<unsigned char>(bytes[0]) == PACKETIA_TURN_IPV4_LOOPBACK_FIRST_OCTET;
                };
            }
            turn_server_ = std::make_shared<protocol::TurnServer>(event_loop_->GetTaskScheduler(), std::move(options),
                [username, password](std::string_view user, std::string& out) {
                    if (user != username) return false;
                    out = password;
                    return true;
                });
            if (!turn_server_->Start()) return false;
            LOG_INFO("TURN started in Packetia on ", turn_server_->LocalAddress().ToString());
            return true;
        },
        [this] {
            if (turn_server_) turn_server_->Stop();
            turn_server_.reset();
        });
}
}
