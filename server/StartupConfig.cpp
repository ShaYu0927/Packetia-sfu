#include "StartupConfig.h"
#include <cstdlib>
#include <chrono>
#include <csignal>
#include <iostream>
#include <stdexcept>
#include <thread>
#include <utility>
#include <vector>

#if defined(__APPLE__)
#include <mach-o/dyld.h>
#elif defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

namespace server
{
namespace
{
volatile std::sig_atomic_t stopping = 0;
void StopSignal(int) { stopping = 1; }
}

StartupOptions ParseStartupOptions(int argc, const char* const* argv)
{
    StartupOptions options;
    for (int i = 1; i < argc; ++i)
    {
        const std::string argument = argv[i];
        if (argument == "--help" || argument == "-h") options.help = true;
        else if (argument == "--check-config") options.check_config = true;
        else if (argument == "--config" || argument == "-c")
        {
            if (!options.config_path.empty())
                throw std::invalid_argument("--config may only be specified once");
            if (++i == argc || !argv[i][0] || argv[i][0] == '-')
                throw std::invalid_argument("--config requires a configuration file path");
            options.config_path = argv[i];
        }
        else throw std::invalid_argument("unknown option: " + argument + "; use --help");
    }
    return options;
}

std::filesystem::path ExecutableDirectory(const char* argv0)
{
    namespace fs = std::filesystem;
#if defined(__linux__)
    std::error_code error;
    const auto executable = fs::read_symlink("/proc/self/exe", error);
    if (!error) return executable.parent_path();
#elif defined(__APPLE__)
    uint32_t size = 0;
    _NSGetExecutablePath(nullptr, &size);
    std::vector<char> buffer(size);
    if (_NSGetExecutablePath(buffer.data(), &size) == 0)
        return fs::canonical(buffer.data()).parent_path();
#elif defined(_WIN32)
    std::vector<wchar_t> buffer(32768);
    const auto size = GetModuleFileNameW(nullptr, buffer.data(), static_cast<DWORD>(buffer.size()));
    if (size && size < buffer.size())
        return fs::path(std::wstring(buffer.data(), size)).parent_path();
#endif
    return fs::absolute(argv0).parent_path();
}

StartupConfig LoadStartupConfig(const StartupOptions& options,
                               const std::filesystem::path& executable_directory)
{
    namespace fs = std::filesystem;
    auto path = options.config_path;
    if (path.empty())
    {
        if (const char* environment = std::getenv("PACKETIA_CONFIG"))
        {
            if (!*environment) throw std::invalid_argument("PACKETIA_CONFIG must not be empty");
            path = environment;
        }
        else
        {
            for (const auto& candidate : {executable_directory / "packetia.json",
                                         fs::current_path() / "packetia.json",
                                         fs::current_path() / "config" / "packetia.json"})
            {
                if (fs::exists(candidate))
                {
                    path = candidate.string();
                    break;
                }
            }
        }
    }
    if (!path.empty()) path = fs::absolute(path).lexically_normal().string();
    return {ServerConfig::FromEnvironment(path), path};
}

std::optional<ServerConfig> PrepareStartup(int argc, const char* const* argv)
{
    const auto options = ParseStartupOptions(argc, argv);
    if (options.help)
    {
        std::cout << "Usage: Packetia [--config FILE] [--check-config]\n"
                  << "Start the single-process server.\n"
                  << "Config order: --config, PACKETIA_CONFIG, packetia.json beside the executable,\n"
                  << "              ./packetia.json, ./config/packetia.json, built-in defaults.\n"
                  << "  -c, --config FILE  Use this configuration file\n"
                  << "  --check-config     Validate configuration without starting services\n"
                  << "  -h, --help         Show this help\n";
        return std::nullopt;
    }
    auto startup = LoadStartupConfig(options, ExecutableDirectory(argv[0]));
    std::cout << "Config: " << (startup.config_path.empty() ? "built-in defaults" : startup.config_path) << '\n'
              << "Working directory: " << std::filesystem::current_path().string() << '\n';
    if (options.check_config)
    {
        std::cout << "Configuration is valid. No services started.\n";
        return std::nullopt;
    }
    stopping = 0;
    std::signal(SIGINT, StopSignal);
    std::signal(SIGTERM, StopSignal);
    return std::move(startup.config);
}

void PrintStartupSummary(const ServerConfig& config)
{
    std::cout << "Packetia started (single process).\n";
    if (config.http.enabled)
    {
        const auto host = config.http.listen_ip.find(':') == std::string::npos
            ? config.http.listen_ip : "[" + config.http.listen_ip + "]";
        std::cout << "HTTP API: http://" << host << ':' << config.http.listen_port << "/api/v1/\n"
                  << "Account registration: " << (config.account.registration_enabled ? "enabled" : "disabled") << '\n'
                  << "Account database: " << std::filesystem::absolute(config.account.database_path).string() << '\n';
    }
    else std::cout << "HTTP API: disabled (set account.enabled and http.enabled in the configuration).\n";
    std::cout << "Press Ctrl+C to stop.\n" << std::flush;
}

void WaitForShutdown()
{
    while (!stopping) std::this_thread::sleep_for(std::chrono::milliseconds(100));
}
}
