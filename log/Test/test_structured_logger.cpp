#include "logger.h"
#include "third/nlohmann/json.hpp"
#include <set>
#include <vector>
#include <limits>

#define CHECK(value) do { if (!(value)) throw std::runtime_error(#value); } while (false)
using Json = nlohmann::json;

namespace
{
struct Directory
{
    std::filesystem::path path = std::filesystem::temp_directory_path() /
        ("packetia-logger-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    Directory() { std::filesystem::create_directory(path); }
    ~Directory() { std::filesystem::remove_all(path); }
};

struct Environment
{
    std::string key;
    bool existed;
    std::string previous;
    Environment(const char* name, const char* value) : key(name), existed(std::getenv(name) != nullptr),
        previous(existed ? std::getenv(name) : "") { Set(value); }
    void Set(const char* value)
    {
#ifdef _WIN32
        _putenv_s(key.c_str(), value ? value : "");
#else
        if (value) setenv(key.c_str(), value, 1);
        else unsetenv(key.c_str());
#endif
    }
    ~Environment() { Set(existed ? previous.c_str() : nullptr); }
};

std::vector<Json> Read(const std::filesystem::path& path)
{
    std::ifstream input(path);
    std::vector<Json> records;
    for (std::string line; std::getline(input, line);) records.push_back(Json::parse(line));
    return records;
}

void Escaping()
{
    std::string message = "中文\"quoted\"\\\n\r\t";
    for (int byte = 0; byte < 32; ++byte) message += static_cast<char>(byte);
    const auto line = packetia::logging::FormatRecord(LogFormat::Json, "sfu", "node\"a", "/a/file.cpp", 42,
                                                     BaseLogger::ERR, message);
    CHECK(line.find('\n') == std::string::npos && line.find('\r') == std::string::npos);
    const auto record = Json::parse(line);
    CHECK(record.at("message") == message && record.at("node_id") == "node\"a");
    CHECK(record.at("service") == "sfu" && record.at("file") == "file.cpp" && record.at("line") == 42);
    CHECK(record.at("level") == "ERROR" && record.at("pid").get<int>() > 0);
    CHECK(!record.at("tid").get<std::string>().empty());
    CHECK(record.at("timestamp").get<std::string>().back() == 'Z');
    const auto text = packetia::logging::FormatRecord(LogFormat::Text, "control", "", "file.cpp", 1, 2, message);
    CHECK(text.find('\n') == std::string::npos && text.find("service=\"control\"") != std::string::npos);
}

void QueueAndReadiness(const std::filesystem::path& path)
{
    LoggerOptions options;
    options.to_console = false;
    options.format = LogFormat::Json;
    options.service = "sfu";
    options.node_id = "sfu-a";
    options.max_queue_bytes = 2048;
    {
        AsyncFileLogger logger(path.string(), options);
        logger.Write(__FILE__, __LINE__, BaseLogger::DEBUG, "filtered");
        logger.Write(__FILE__, __LINE__, BaseLogger::INFO, "first\nsecond");
        logger.log(__FILE__, __LINE__, BaseLogger::ERR, "external message");
        logger.flush();
        CHECK(logger.stats().accepted == 2 && logger.stats().written == 2);
        logger.set_min_level(BaseLogger::ERR);
        logger.Ready(__FILE__, __LINE__);
        logger.flush();
        logger.Write(__FILE__, __LINE__, BaseLogger::ERR, std::string(4096, 'x'));
        CHECK(logger.stats().dropped == 1 && logger.stats().pending_bytes <= options.max_queue_bytes);
    }
    const auto records = Read(path);
    CHECK(records.size() == 4);
    CHECK(records[0].at("message") == "first\nsecond");
    CHECK(records[1].at("message") == "external message");
    CHECK(records[2].at("event") == "process_ready");
    CHECK(records[3].at("event") == "logger_dropped");
    for (const auto& record : records)
        CHECK(record.at("service") == "sfu" && record.at("node_id") == "sfu-a");
}

void ConcurrentOutput(const std::filesystem::path& path)
{
    LoggerOptions options;
    options.to_console = false;
    options.format = LogFormat::Json;
    {
        AsyncFileLogger logger(path.string(), options);
        std::vector<std::thread> producers;
        for (int id = 0; id < 4; ++id)
            producers.emplace_back([&, id] {
                for (int index = 0; index < 40; ++index)
                    logger.Write(__FILE__, __LINE__, BaseLogger::INFO, std::to_string(id) + ":" + std::to_string(index));
            });
        for (auto& thread : producers) thread.join();
        logger.flush();
        CHECK(logger.stats().accepted == 160 && logger.stats().written == 160 && logger.stats().dropped == 0);
    }
    std::set<std::string> messages;
    for (const auto& record : Read(path)) messages.insert(record.at("message"));
    CHECK(messages.size() == 160);
}

void Rotation(const std::filesystem::path& path)
{
    LoggerOptions options;
    options.to_console = false;
    options.format = LogFormat::Json;
    options.max_file_bytes = 400;
    options.backup_files = 12;
    {
        AsyncFileLogger logger(path.string(), options);
        for (int index = 0; index < 10; ++index)
            logger.Write(__FILE__, __LINE__, BaseLogger::INFO, "rotate", index);
    }
    auto records = Read(path);
    for (int index = 1; index <= 12; ++index)
    {
        const auto part = Read(path.string() + "." + std::to_string(index));
        records.insert(records.end(), part.begin(), part.end());
    }
    CHECK(records.size() == 10 && std::filesystem::exists(path.string() + ".1"));
}

void TypedEventFields(const std::filesystem::path& path)
{
    LoggerOptions options;
    options.to_console = false;
    options.format = LogFormat::Json;
    {
        AsyncFileLogger logger(path.string(), options);
        logger.WriteEvent(__FILE__, __LINE__, BaseLogger::INFO, "qos_sample", "diagnostic",
            {{"transport_id", uint64_t(-1)}, {"counter", int64_t(-7)}, {"valid", true},
             {"missing", std::optional<double>{}}, {"bad", std::numeric_limits<double>::infinity()},
             {"label", "quote\"\nline"}, {"rate", 123.456789}, {"event", "nested"}});
        logger.set_min_level(BaseLogger::ERR);
        logger.WriteEvent(__FILE__, __LINE__, BaseLogger::INFO, "hidden", "filtered", {});
    }
    const auto records = Read(path);
    CHECK(records.size() == 1);
    const auto& record = records.front();
    CHECK(record.at("event") == "qos_sample");
    const auto& data = record.at("data");
    CHECK(data.at("transport_id").get<uint64_t>() == uint64_t(-1));
    CHECK(data.at("counter") == -7 && data.at("valid") == true);
    CHECK(data.at("missing").is_null() && data.at("bad").is_null());
    CHECK(data.at("label") == "quote\"\nline" && data.at("event") == "nested");
    CHECK(std::abs(data.at("rate").get<double>() - 123.456789) < 1e-8);
    const auto text = packetia::logging::FormatRecord(LogFormat::Text, "sfu", "node", __FILE__, __LINE__,
        BaseLogger::INFO, "diagnostic", "qos_sample", {{"label", "a\nb"}});
    CHECK(text.find('\n') == std::string::npos && text.find("data={") != std::string::npos);
}

void EnvironmentAndFiltering()
{
    Environment mode("PACKETIA_LOG_MODE", "console");
    Environment format("PACKETIA_LOG_FORMAT", "json");
    Environment level("PACKETIA_LOG_LEVEL", "error");
    auto& logger = InitializeLogger("test", "node-test");
    CHECK(&InitializeLogger("test", "node-test") == &logger && &GlobalLogger() == &logger);
    bool rejected = false;
    try { InitializeLogger("other"); } catch (const std::logic_error&) { rejected = true; }
    CHECK(rejected);
    int evaluated = 0;
    LOG_DEBUG(++evaluated);
    LOG_INFO(++evaluated);
    CHECK(evaluated == 0);
    LOG_ERROR(++evaluated);
    CHECK(evaluated == 1);
    logger.set_min_level(BaseLogger::DEBUG);
    LOG_DEBUG(++evaluated);
    CHECK(evaluated == 2);
    logger.flush();
    CHECK(logger.stats().accepted == 2);
    for (const char* key : {"PACKETIA_LOG_MODE", "PACKETIA_LOG_FORMAT", "PACKETIA_LOG_LEVEL"})
    {
        Environment invalid(key, "invalid");
        rejected = false;
        try { LoggerOptionsFromEnvironment("test", "node-test"); }
        catch (const std::invalid_argument&) { rejected = true; }
        CHECK(rejected);
    }
}
}

int main()
try
{
    Directory directory;
    Escaping();
    QueueAndReadiness(directory.path / "queue.log");
    ConcurrentOutput(directory.path / "concurrent.log");
    Rotation(directory.path / "rotation.log");
    TypedEventFields(directory.path / "fields.log");
    EnvironmentAndFiltering();
    std::cout << "PASS structured logging, filtering, identity, readiness, concurrency, drops and rotation\n";
}
catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
