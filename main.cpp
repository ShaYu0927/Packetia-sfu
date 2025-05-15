#include <iostream>
#include <thread>
#include "TcpServer.h"
#include "EventLoop.h"
#include <string.h>

int main() {
    EventLoop loop;

	TcpServer server(&loop);
	std::string ip = "0.0.0.0";
	uint16_t port = 8888;
	server.Start(ip, port);

	loop.Loop();

	while (true) {
		std::this_thread::sleep_for(std::chrono::milliseconds(100));
	}

    return 0;
}



