#include "ServerConfig.h"

#include <cstdlib>

namespace server
{
ServerConfig ServerConfig::FromEnvironment()
{
    ServerConfig config;
    if (const char* enabled = std::getenv("PACKETIA_RECORDING"))
    {
        config.recording_enabled = std::string(enabled) != "0";
    }
    if (const char* directory = std::getenv("PACKETIA_RECORD_DIR"))
    {
        config.recording.directory = directory;
    }
    return config;
}
}
