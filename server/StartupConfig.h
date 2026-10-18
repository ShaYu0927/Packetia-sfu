#pragma once

#include "ServerConfig.h"
#include <filesystem>
#include <optional>
#include <string>

namespace server
{
struct StartupOptions
{
    std::string config_path;
    bool help = false;
    bool check_config = false;
};

struct StartupConfig
{
    ServerConfig config;
    // Empty means built-in defaults; otherwise this is an absolute path.
    std::string config_path;
};

StartupOptions ParseStartupOptions(int argc, const char* const* argv);
std::filesystem::path ExecutableDirectory(const char* argv0);
StartupConfig LoadStartupConfig(const StartupOptions& options,
                               const std::filesystem::path& executable_directory);
std::optional<ServerConfig> PrepareStartup(int argc, const char* const* argv);
void PrintStartupSummary(const ServerConfig& config);
void WaitForShutdown();
}
