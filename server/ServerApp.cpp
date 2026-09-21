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
#include "AIService/AIService.h"
#include "AIService/UnavailableModelProvider.h"
#include "RecordService/RecordingService.h"
#include "core/EncodedFrameRouter.h"
#include "logger.h"

#ifdef PACKETIA_WITH_LIBWEBSOCKETS
#include "websocket/WsServer.h"
#endif

#include <utility>

namespace server
{
ServerApp::ServerApp(ServerConfig config)
    : config_(std::move(config)),
      event_loop_(std::make_shared<EventLoop>(config_.io_threads)),
      frame_router_(std::make_shared<media::EncodedFrameRouter>())
{
    ConfigureServices();
}

ServerApp::~ServerApp()
{
    Stop();
}

bool ServerApp::Start()
{
    return launcher_.StartAll();
}

void ServerApp::Run()
{
    event_loop_->Loop();
}

void ServerApp::Stop() noexcept
{
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
    if (config_.recording_enabled)
    {
        launcher_.AddService(SERVICE_RECORD,
            std::make_shared<service::RecordingService>(frame_router_, config_.recording));
    }
    launcher_.AddService(SERVICE_AI,
        std::make_shared<service::ai::AIService>(
            std::make_shared<service::ai::UnavailableModelProvider>(), frame_router_));
}

void ServerApp::AddNetworkServices()
{
    const auto& ip = config_.listen_ip;
    auto* loop = event_loop_.get();
    launcher_.AddIpPortService<RtspServer>(SERVICE_RTSP, ip, config_.rtsp_port, loop);
    launcher_.AddIpPortService<SipServer>(SERVICE_SIP, ip, config_.sip_port, loop);
    launcher_.AddIpPortService<protocol::rtmp::RtmpServer>(
        SERVICE_RTMP, ip, config_.rtmp_port, loop);

    auto udp = launcher_.AddIpPortService<network::UdpServer>(
        SERVICE_UDP, ip, config_.udp_port, loop);
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
}
