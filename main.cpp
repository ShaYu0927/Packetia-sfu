#include "RtspServer.h"
#include "EventLoop.h"

int main() {
    // 创建事件循环
    auto event_loop = std::make_shared<EventLoop>(1);

    // 创建 RTSP 服务器
    auto rtsp_server = std::make_shared<RtspServer>(event_loop.get());

    // 启动 RTSP 服务器，监听指定 IP 和端口
    if (rtsp_server->Start("0.0.0.0", 554)) {
        std::cout << "RTSP Server started on port 554" << std::endl;
    } else {
        std::cout << "RTSP Server start failed" << std::endl;
        return -1;
    }

    // 阻塞主线程，防止退出
    while (true) {
        std::this_thread::sleep_for(std::chrono::seconds(10));
    }
    return 0;
}