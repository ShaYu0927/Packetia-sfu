#include "StartupConfig.h"
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <optional>
#include <stdexcept>
#include <vector>

namespace
{
namespace fs = std::filesystem;
void Check(bool condition, const char* message)
{
    if (!condition) throw std::runtime_error(message);
}

void Set(const char* name, const char* value)
{
#ifdef _WIN32
    if (_putenv_s(name, value ? value : "") != 0)
#else
    if ((value ? setenv(name, value, 1) : unsetenv(name)) != 0)
#endif
        throw std::runtime_error("cannot update test environment");
}

struct SavedEnvironment
{
    const char* name;
    std::optional<std::string> value;
    explicit SavedEnvironment(const char* key) : name(key)
    {
        if (const char* original = std::getenv(name)) value = original;
        Set(name, nullptr);
    }
    ~SavedEnvironment() { Set(name, value ? value->c_str() : nullptr); }
};

struct Workspace
{
    fs::path original = fs::current_path();
    fs::path parent = fs::temp_directory_path();
    fs::path root = parent / ("packetia-startup-" + std::to_string(
        std::chrono::steady_clock::now().time_since_epoch().count()));
    Workspace()
    {
        Check(fs::create_directory(root), "cannot create isolated test directory");
        fs::create_directory(root / "bin");
        fs::current_path(root);
    }
    ~Workspace()
    {
        fs::current_path(original);
        // Only remove this test's newly created child of the temporary directory.
        if (root.parent_path() == parent) fs::remove_all(root);
    }
};

void Write(const fs::path& path, const char* json)
{
    std::ofstream file(path);
    file << json;
    Check(static_cast<bool>(file), "cannot write test configuration");
}

template<class Callback> void Reject(Callback callback, const char* message)
{
    bool rejected = false;
    try { callback(); } catch (const std::exception&) { rejected = true; }
    Check(rejected, message);
}

void CommandLine()
{
    const char* arguments[] = {"Packetia", "--config", "custom.json", "--check-config"};
    const auto options = server::ParseStartupOptions(4, arguments);
    Check(options.config_path == "custom.json" && options.check_config && !options.help,
          "command line options ignored");
    const char* help[] = {"Packetia", "--help"};
    Check(server::ParseStartupOptions(2, help).help, "help option ignored");
    for (const auto& invalid : std::vector<std::vector<const char*>>{
             {"Packetia", "--config"}, {"Packetia", "--config", ""},
             {"Packetia", "--config", "--check-config"}, {"Packetia", "--unknown"},
             {"Packetia", "--config", "a.json", "-c", "b.json"}})
        Reject([&] { server::ParseStartupOptions(static_cast<int>(invalid.size()), invalid.data()); },
               "invalid command line accepted");
}

void ConfigurationSelection()
{
    Workspace workspace;
    const auto binary_directory = workspace.root / "bin";
    auto load = [&](const server::StartupOptions& options = {}) {
        return server::LoadStartupConfig(options, binary_directory);
    };
    Check(load().config_path.empty() && !load().config.http.enabled,
          "missing files must preserve built-in defaults");

    fs::create_directory("config");
    Write("config/packetia.json", R"({"server":{"rtsp_port":18554}})");
    Check(load().config.rtsp_port == 18554, "project configuration not discovered");
    Write("packetia.json", R"({"server":{"rtsp_port":18555}})");
    Check(load().config.rtsp_port == 18555, "working-directory configuration precedence failed");
    Write(binary_directory / "packetia.json", R"({"server":{"rtsp_port":18556}})");
    Check(load().config.rtsp_port == 18556, "executable configuration precedence failed");

    Write("environment.json", R"({"server":{"rtsp_port":18557},"services":{"ai":true}})");
    Set("PACKETIA_CONFIG", "environment.json");
    Set("PACKETIA_AI", "false");
    Check(load().config.rtsp_port == 18557 && !load().config.ai_enabled,
          "environment configuration or feature override ignored");

    Write("explicit.json", R"({"server":{"rtsp_port":18558},"account":{"enabled":true,
          "database_path":"data/accounts.db"},"http":{"enabled":true}})");
    server::StartupOptions options;
    options.config_path = "explicit.json";
    const auto explicit_config = load(options);
    Check(explicit_config.config.rtsp_port == 18558 && explicit_config.config.http.enabled &&
          fs::path(explicit_config.config_path).is_absolute(), "explicit configuration precedence failed");
    Check(explicit_config.config.account.database_path == "data/accounts.db" && !fs::exists("data"),
          "loading must preserve relative data paths without creating runtime data");

    options.config_path = "missing.json";
    Reject([&] { load(options); }, "missing explicit file silently fell back");
    Set("PACKETIA_CONFIG", "missing.json");
    Reject([&] { load(); }, "missing environment file silently fell back");
    Set("PACKETIA_CONFIG", nullptr);
    Write(binary_directory / "packetia.json", "{invalid");
    Reject([&] { load(); }, "invalid default file silently fell back");
}
}

int main(int argc, char** argv)
{
    try
    {
        std::vector<SavedEnvironment> environment;
        // Reserve before construction so guards are never copied during growth.
        environment.reserve(11);
        for (const char* key : {"PACKETIA_CONFIG", "PACKETIA_RECORDING", "PACKETIA_AI",
             "PACKETIA_CONFERENCE_MIX", "PACKETIA_WEBRTC", "PACKETIA_TURN",
             "PACKETIA_WEBRTC_PUBLIC_IP", "PACKETIA_WEBRTC_TOKEN", "PACKETIA_WEBRTC_API_KEY",
             "PACKETIA_WEBRTC_API_SECRET", "PACKETIA_RECORD_DIR"}) environment.emplace_back(key);
        CommandLine();
        ConfigurationSelection();
        if (argc == 2)
        {
            const auto defaults = server::ServerConfig::FromFile(argv[1]);
            Check(defaults.http.enabled && defaults.account.enabled && defaults.account.registration_enabled &&
                  defaults.http.listen_ip == "127.0.0.1" && defaults.http.listen_port == 8081 &&
                  defaults.rtsp_port >= 1024 && defaults.sip_port >= 1024,
                  "shipped configuration must support unprivileged local HTTP startup");
        }
        Check(fs::is_directory(server::ExecutableDirectory("Packetia")), "executable discovery failed");
        std::cout << "Startup configuration tests passed\n";
        return 0;
    }
    catch (const std::exception& error)
    {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
