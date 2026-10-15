#ifdef PACKETIA_CONTROL_PROCESS
#include "ControlApp.h"
#else
#include "SfuApp.h"
#endif
#include "utils/ThreadManager.h"
#include <csignal>
#include <chrono>
#include <iostream>
#include <thread>

namespace
{
volatile std::sig_atomic_t stopping = 0;
void StopSignal(int) { stopping = 1; }
}
int main(int argc, char** argv)
{
    if (argc != 2) { std::cerr << "Usage: " << argv[0] << " CONFIG.json\n"; return 2; }
    std::signal(SIGINT, StopSignal);
    std::signal(SIGTERM, StopSignal);
    try
    {
        auto main_thread = utils::ThreadManager::Instance().TrackCurrent({"main", "main", utils::ThreadModel::Main});
#ifdef PACKETIA_CONTROL_PROCESS
        server::cluster::ControlApp app(argv[1]);
#else
        server::cluster::SfuApp app(argv[1]);
#endif
        if (!app.Start()) { std::cerr << "Process startup failed; check configuration, ports and control service.\n"; return 1; }
        std::cout << "PACKETIA_READY" << std::endl;
        while (!stopping) std::this_thread::sleep_for(std::chrono::milliseconds(100));
        app.Stop();
        return 0;
    }
    catch (const std::exception&) { std::cerr << "Process startup/runtime failed; check configuration and required environment variables.\n"; return 1; }
}
