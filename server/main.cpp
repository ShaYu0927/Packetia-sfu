#include "RtspServer.h"
#include "EventLoop.h"
#include "logger.h"
#include "UdpServer.h"
#include "UdpSession.h"
#include "EndpointBase.h"
#include "IWorkerModule.h"
#include "ServerLauncher.h"

int main()
{
    server::ServerLauncher launcher;

    WorkerModuleRegistry registry;

    auto media_module = std::make_shared<MediaWorkerModule>();
    registry.Add(media_module);

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

        }
    );

    launcher.AddCustomService(
        "EndpointWorkerPool",
        []() -> bool {
            auto handler = std::make_shared<utils::EndpointJobHandler>(
                &utils::EndpointManager::Instance()
            );

            if (WorkerService::create_pool("endpoint_pool", 4, handler, 4096) != 0)
            {
                LOG_ERROR("create worker pool failed");
                return false;
            }

            return true;
        },
        []() {
            WorkerService::destroy_pool("endpoint_pool", true);
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

    auto rtsp_server = launcher.AddIpPortService<RtspServer>(
        "RtspServer",
        "0.0.0.0",
        554,
        event_loop.get()
    );

    auto udp_server = launcher.AddIpPortService<network::UdpServer>(
        "UdpServer",
        "0.0.0.0",
        9000,
        event_loop.get()
    );

    auto mux_handler = std::make_shared<network::UdpMuxHandler>(udp_server.get());
    udp_server->SetHandler(mux_handler);

    if (!launcher.StartAll())
    {
        return -1;
    }

    event_loop->Loop();

    launcher.StopAll();

    return 0;
}