#pragma once

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <stdexcept>
#include <string>
#include <utility>

enum class LogFormat { Text, Json };

struct LoggerOptions
{
    bool to_console = true;
    bool to_file = true;
    size_t max_queue_messages = 8192;
    size_t max_queue_bytes = 8 * 1024 * 1024;
    std::uintmax_t max_file_bytes = 16 * 1024 * 1024;
    size_t backup_files = 3;
    std::chrono::milliseconds flush_interval{200};
    LogFormat format = LogFormat::Text;
    int min_level = 2; // INFO; matches ILogger::Level.
    std::string service = "packetia";
    std::string node_id;
};

inline LoggerOptions LoggerOptionsFromEnvironment(std::string service, std::string node_id)
{
    LoggerOptions options;
    options.service = std::move(service);
    options.node_id = std::move(node_id);
    if (const char* raw = std::getenv("PACKETIA_LOG_MODE"))
    {
        const std::string mode(raw);
        if (mode != "console" && mode != "file" && mode != "both")
            throw std::invalid_argument("PACKETIA_LOG_MODE expects console, file or both");
        options.to_file = mode != "console";
        options.to_console = mode != "file";
    }
    if (const char* raw = std::getenv("PACKETIA_LOG_FORMAT"))
    {
        const std::string format(raw);
        if (format != "text" && format != "json")
            throw std::invalid_argument("PACKETIA_LOG_FORMAT expects text or json");
        options.format = format == "json" ? LogFormat::Json : LogFormat::Text;
    }
    if (const char* raw = std::getenv("PACKETIA_LOG_LEVEL"))
    {
        const std::string level(raw);
        if (level == "debug") options.min_level = 1;
        else if (level == "info") options.min_level = 2;
        else if (level == "error") options.min_level = 3;
        else throw std::invalid_argument("PACKETIA_LOG_LEVEL expects debug, info or error");
    }
    return options;
}
