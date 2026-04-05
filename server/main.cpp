#include "RtspServer.h"
#include "EventLoop.h"
#include "logger.h"
#include "UdpServer.h"
#include "UdpSession.h"
#include "EndpointBase.h"
#include "IWorkerModule.h"

int main()
{
    WorkerModuleRegistry registry;

    registry.Add(std::make_shared<MediaWorkerModule>());

    int ret = registry.RegisterAll();
    if (ret != 0)
    {
        std::cerr << "worker modules init failed\n";
        return -1;
    }

    auto handler = std::make_shared<utils::EndpointJobHandler>(&utils::EndpointManager::Instance());
    if (WorkerService::create_pool("endpoint_pool", 4, handler, 4096) != 0)
    {
        LOG_ERROR("create worker pool failed");
        return -1;
    }

    auto event_loop = std::make_shared<EventLoop>(1);
    if (!event_loop->Start())
    {
        LOG_ERROR("event loop start failed");
        return -1;
    }

    auto rtsp_server = std::make_shared<RtspServer>(event_loop.get());
    if (!rtsp_server->Start("0.0.0.0", 554))
    {
        LOG_ERROR("RTSP start failed");
        return -1;
    }

    auto udp_server = std::make_shared<network::UdpServer>(event_loop.get());
    auto mux_handler = std::make_shared<network::UdpMuxHandler>(udp_server.get());
    udp_server->SetHandler(mux_handler);
    if (!udp_server->Start("0.0.0.0", 9000))
    {
        LOG_ERROR("UDP start failed");
        return -1;
    }

    event_loop->Loop();

    WorkerService::destroy_pool("endpoint_pool", true);
    return 0;
}