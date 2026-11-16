#ifdef PACKETIA_CONTROL_PROCESS
#include "ControlApp.h"
#else
#include "SfuApp.h"
#endif
#include "ThreadManager.h"
#include "logger.h"
#ifndef PACKETIA_CONTROL_PROCESS
#include "third/nlohmann/json.hpp"
#include <fstream>
#endif
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
#ifdef PACKETIA_CONTROL_PROCESS
        auto& logger = InitializeLogger("control");
#else
        // Read identity before application constructors can emit their first log.
        std::ifstream input(argv[1]);
        const auto config = nlohmann::json::parse(input);
        auto& logger = InitializeLogger("sfu", config.at("node_id").get<std::string>());
#endif
        auto main_thread = utils::ThreadManager::Instance().TrackCurrent({"main", "main", utils::ThreadModel::Main});
#ifdef PACKETIA_CONTROL_PROCESS
        server::cluster::ControlApp app(argv[1]);
#else
        server::cluster::SfuApp app(argv[1]);
#endif
        if (!app.Start())
        {
            LOG_ERROR("Process startup failed; check configuration, ports and control service.");
            logger.flush();
            return 1;
        }
        logger.Ready(__FILE__, __LINE__);
        logger.flush();
        while (!stopping) std::this_thread::sleep_for(std::chrono::milliseconds(100));
        app.Stop();
        LOG_INFO("Process stopped");
        logger.flush();
        return 0;
    }
    catch (const std::exception&)
    {
        try
        {
            LOG_ERROR("Process startup/runtime failed; check configuration and required environment variables.");
            GlobalLogger().flush();
        }
        catch (...) { std::cerr << "Process failed; check configuration and logging options.\n"; }
        return 1;
    }
}
