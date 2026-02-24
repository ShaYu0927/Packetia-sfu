#include "RtspServer.h"
#include "EventLoop.h"
#include "logger.h"
#include "UdpServer.h"
#include "UdpSession.h"

int main() 
{
    auto event_loop = std::make_shared<EventLoop>(1);
    auto rtsp_server = std::make_shared<RtspServer>(event_loop.get());
    if (!rtsp_server->Start("0.0.0.0", 554)) 
    {
        LOG_ERROR("RTSP start failed");
        return -1;
    }
    auto udp_server = std::make_shared<network::UdpServer>(event_loop.get());
    auto handler = std::make_shared<network::UdpMuxHandler>(udp_server.get());
    udp_server->SetHandler(handler);

    if (!udp_server->Start("0.0.0.0", 9000)) 
    {
        LOG_ERROR("UDP start failed");
        return -1;
    }

    LOG_INFO("RTSP + UDP started");

    while (true) 
    {
        std::this_thread::sleep_for(std::chrono::seconds(10));
    }

    return 0;
}