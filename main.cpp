#include "RtspServer.h"
#include "EventLoop.h"
#include "logger.h"

int main() 
{
    auto event_loop = std::make_shared<EventLoop>(1);
    auto rtsp_server = std::make_shared<RtspServer>(event_loop.get());

    if (rtsp_server->Start("0.0.0.0", 554)) 
    {
        LOG_INFO("RTSP Server started on port 554");
    } 
    else 
    {
        LOG_ERROR("RTSP Server start failed");
        return -1;
    }

    while (true) 
    {
        std::this_thread::sleep_for(std::chrono::seconds(10));
    }
    return 0;
}