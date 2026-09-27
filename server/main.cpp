#include "ServerApp.h"
#include <exception>
#include <iostream>

int main()
{
    try {
        server::ServerApp app(server::ServerConfig::FromEnvironment());
        if (!app.Start())
        {
            return -1;
        }
        app.Run();
        app.Stop();
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "Packetia startup/runtime error: " << error.what() << '\n';
        return 1;
    }
}
