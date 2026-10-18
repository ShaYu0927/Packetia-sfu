#include "ServerApp.h"
#include "StartupConfig.h"
#include <exception>
#include <iostream>

int main(int argc, char** argv)
{
    try {
        auto config = server::PrepareStartup(argc, argv);
        if (!config) return 0;
        auto main_thread = utils::ThreadManager::Instance().TrackCurrent(
            {"main", "main", utils::ThreadModel::Main});
        server::ServerApp app(*config);
        if (!app.Start())
        {
            std::cerr << "Packetia failed to start; check the service errors above.\n";
            return 1;
        }
        server::PrintStartupSummary(*config);
        server::WaitForShutdown();
        app.Stop();
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "Packetia startup/runtime error: " << error.what() << '\n';
        return 1;
    }
}
