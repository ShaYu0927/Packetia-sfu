#include "ServerApp.h"
#include "ServiceNames.h"
#include "WorkerSetup.h"

#include "EventLoop.h"
#include "RtspServer.h"
#include "RtspMediaSession.h"
#include "SipServer.h"
#include "rtmp_server.h"
#include "UdpServer.h"
#include "UdpSession.h"
#include "TurnServer.h"
#include "StringUtil.h"
#include "AIService/AIService.h"
#include "AIService/UnavailableModelProvider.h"
#include "RecordService/RecordingService.h"
#include "core/EncodedFrameRouter.h"
#include "logger.h"

#ifdef PACKETIA_WITH_LIBWEBSOCKETS
#include "websocket/WsServer.h"
#include "WebRtcService.h"
#endif

#include <stdexcept>
#include <cstdlib>
#include <utility>

namespace server
{
ServerApp::ServerApp(ServerConfig config, std::shared_ptr<service::IRecordingEventSink> recording_events)
    : config_(std::move(config)),
      settings_(std::make_shared<config::ConfigStore>(config_)),
      recording_events_(std::move(recording_events)),
      event_loop_(std::make_shared<EventLoop>(config_.io_threads)),
      frame_router_(std::make_shared<media::EncodedFrameRouter>()),
      mix_output_router_(std::make_shared<media::EncodedFrameRouter>()),
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
    return launcher_.StartAll();
}

void ServerApp::Run()
{
    event_loop_->Loop();
}

void ServerApp::Stop() noexcept
{
    std::lock_guard<std::mutex> lock(lifecycle_mutex_);
    launcher_.StopAll();
}

void ServerApp::ConfigureServices()
{
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

    AddMediaServices();
    AddWorkerPools(launcher_);
    AddNetworkServices();
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
}

bool ServerApp::SetServiceEnabled(service::ServiceType type, bool enabled)
{
    std::lock_guard<std::mutex> lock(lifecycle_mutex_);
    const char* name = nullptr;
    config::Feature feature;
    switch (type) 
    {
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
            if (username.empty() || username.size() > 512 || password.empty() || password.size() > 763 ||
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
            if (config.local_test)
            {
                options.allow_peer = [](const network::SocketAddr& peer) {
                    if (peer.IsV6())
                        return IN6_IS_ADDR_LOOPBACK(&reinterpret_cast<const sockaddr_in6*>(&peer.ss)->sin6_addr);
                    const auto bytes = peer.IPv4Bytes();
                    return bytes.size() == 4 && static_cast<unsigned char>(bytes[0]) == 127;
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
