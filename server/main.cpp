#include "RtspServer.h"
#include "SipServer.h"
#include "EventLoop.h"
#include "logger.h"
#include "UdpServer.h"
#include "UdpSession.h"
#include "IWorkerModule.h"
#include "ServerLauncher.h"
#include "websocket/WsServer.h"

#include <algorithm>
#include <thread>

int main()
{
    server::ServerLauncher launcher;
    WorkerModuleRegistry registry;

    auto media_module = std::make_shared<MediaWorkerModule>();
    registry.Add(media_module);
    registry.Add(std::make_shared<EndpointWorkerModule>());
    registry.Add(std::make_shared<FunctionWorkerModule>("recording", 2, 2048, ShardedWorkerPool::DropPolicy::DropTail));

    const auto hardware_threads = std::thread::hardware_concurrency();
    const auto transcode_threads = std::clamp<std::size_t>(hardware_threads / 2, 1, 4);
    registry.Add(std::make_shared<FunctionWorkerModule>("transcode", transcode_threads, 512, ShardedWorkerPool::DropPolicy::DropTail));

    launcher.AddCustomService(
        "WorkerModuleRegistry",
        [&registry]() -> bool {
            int ret = registry.RegisterAll();
            if (ret != 0)
            {
                LOG_ERROR("worker modules init failed");
                return false;
            }

            return true;
        },
        [&registry]() {
            registry.UnregisterAll(true);
        }
    );

    auto event_loop = std::make_shared<EventLoop>(1);

    launcher.AddCustomService(
        "EventLoop",
        [event_loop]() -> bool {
            if (!event_loop->Start())
            {
                LOG_ERROR("event loop start failed");
                return false;
            }

            return true;
        },
        [event_loop]() {
        }
    );

    auto rtsp_server = launcher.AddIpPortService<RtspServer>("RtspServer", "0.0.0.0", 554, event_loop.get());
    auto sip_server   = launcher.AddIpPortService<SipServer>("SipServer", "0.0.0.0", 5060, event_loop.get());
    auto udp_server   = launcher.AddIpPortService<network::UdpServer>("UdpServer","0.0.0.0", 9000,  event_loop.get());
    auto mux_handler                              = std::make_shared<network::UdpMuxHandler>(udp_server.get());
    udp_server->SetHandler(mux_handler);

    auto ws_server = std::make_shared<network::websocket::WsServer>();

    ws_server->SetOnOpen([](const network::websocket::WsConnectionInfo& info) {
        LOG_INFO("ws open, connId=", info.connId);
    });

    ws_server->SetOnMessage(
        [](const std::string& connId, const std::string& message) -> std::string {
            LOG_INFO("ws message, connId=", connId, ", message=", message);
            return R"({"code":0,"msg":"ok"})";
        }
    );

    ws_server->SetOnClose([](const std::string& connId) {
        LOG_INFO("ws close, connId=", connId);
    });

    launcher.AddCustomService(
        "WsServer",
        [ws_server]() -> bool {
            return ws_server->Start("0.0.0.0", 8080, 1);
        },
        [ws_server]() {
            ws_server->Stop();
        }
    );

    if (!launcher.StartAll())
    {
        return -1;
    }

    event_loop->Loop();
    launcher.StopAll();

    return 0;
}
