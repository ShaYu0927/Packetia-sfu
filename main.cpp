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
	if(!server.Start(ip, port))
	{
		std::cout << "Failed to start server on " << ip << ":" << port << std::endl;
		return -1;
	}
	else
	{
		std::cout << "Server started on " << ip << ":" << port << std::endl;
	}

	loop.Loop();
	while(true) {
        std::this_thread::sleep_for(std::chrono::seconds(1));
    }

	return 0;
}



