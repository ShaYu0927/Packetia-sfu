#pragma once

#include "LoggerConfig.h"
#include <cstdio>
#include <ctime>
#include <sstream>
#include <string_view>
#include <thread>
#include <cmath>
#include <optional>
#include <type_traits>
#include <variant>
#include <vector>

#ifdef _WIN32
#include <process.h>
#else
#include <unistd.h>
#endif

namespace packetia::logging
{
// Typed scalar fields keep diagnostics queryable without embedding JSON in message.
struct LogField
{
    using Value = std::variant<std::nullptr_t, bool, int64_t, uint64_t, double, std::string>;
    std::string key;
    Value value;

    static Value Convert(std::nullptr_t) { return nullptr; }
    static Value Convert(bool value) { return value; }
    static Value Convert(double value) { return value; }
    static Value Convert(const char* value) { return value ? Value(std::string(value)) : Value(nullptr); }
    static Value Convert(const std::string& value) { return value; }
    template<typename T, std::enable_if_t<std::is_integral_v<T> && !std::is_same_v<T, bool>, int> = 0>
    static Value Convert(T value)
    {
        if constexpr (std::is_signed_v<T>) return static_cast<int64_t>(value);
        else return static_cast<uint64_t>(value);
    }
    template<typename T>
    static Value Convert(const std::optional<T>& value) { return value ? Convert(*value) : Value(nullptr); }
    template<typename T>
    LogField(std::string name, const T& input) : key(std::move(name)), value(Convert(input)) {}
};
using LogFields = std::vector<LogField>;

inline const char* LevelName(int level)
{
    switch (level)
    {
        case 0: return "TRACE";
        case 1: return "DEBUG";
        case 2: return "INFO";
        case 3: return "ERROR";
        default: return "UNKNOWN";
    }
}

inline std::string Timestamp()
{
    const auto now = std::chrono::system_clock::now();
    const auto millis = std::chrono::duration_cast<std::chrono::milliseconds>(now.time_since_epoch()).count() % 1000;
    const auto seconds = std::chrono::system_clock::to_time_t(now);
    std::tm utc{};
#ifdef _WIN32
    gmtime_s(&utc, &seconds);
#else
    gmtime_r(&seconds, &utc);
#endif
    char buffer[40];
    std::snprintf(buffer, sizeof(buffer), "%04d-%02d-%02dT%02d:%02d:%02d.%03dZ",
        utc.tm_year + 1900, utc.tm_mon + 1, utc.tm_mday, utc.tm_hour, utc.tm_min, utc.tm_sec,
        static_cast<int>(millis));
    return buffer;
}

inline void AppendJsonString(std::string& out, std::string_view value)
{
    constexpr char hex[] = "0123456789abcdef";
    out += '"';
    for (const unsigned char byte : value)
    {
        if (byte == '"' || byte == '\\') { out += '\\'; out += static_cast<char>(byte); }
        else if (byte < 0x20)
        {
            out += "\\u00";
            out += hex[byte >> 4];
            out += hex[byte & 15];
        }
        else out += static_cast<char>(byte);
    }
    out += '"';
}

inline std::string FieldsJson(const LogFields& fields)
{
    std::string out = "{";
    bool first = true;
    for (const auto& field : fields)
    {
        if (!first) out += ',';
        first = false;
        AppendJsonString(out, field.key); out += ':';
        std::visit([&out](const auto& value) {
            using T = std::decay_t<decltype(value)>;
            if constexpr (std::is_same_v<T, std::nullptr_t>) out += "null";
            else if constexpr (std::is_same_v<T, bool>) out += value ? "true" : "false";
            else if constexpr (std::is_same_v<T, std::string>) AppendJsonString(out, value);
            else if constexpr (std::is_floating_point_v<T>)
            {
                if (!std::isfinite(value)) out += "null";
                else { std::ostringstream stream; stream.imbue(std::locale::classic()); stream.precision(15); stream << value; out += stream.str(); }
            }
            else out += std::to_string(value);
        }, field.value);
    }
    out += '}';
    return out;
}

// Text and JSON both keep each application record on a single physical line.
inline std::string FormatRecord(LogFormat format, const std::string& service, const std::string& node_id,
                                const char* file, int line, int level, std::string_view message,
                                const char* event = nullptr, const LogFields& fields = {})
{
    const std::string_view path(file);
    const auto separator = path.find_last_of("/\\");
    const auto source = separator == std::string_view::npos ? path : path.substr(separator + 1);
#ifdef _WIN32
    const auto pid = _getpid();
#else
    const auto pid = getpid();
#endif
    thread_local const std::string tid = [] {
        std::ostringstream stream;
        stream << std::this_thread::get_id();
        return stream.str();
    }();
    std::string out;
    out.reserve(256 + message.size());
    if (format == LogFormat::Json)
    {
        out = "{\"timestamp\":";
        AppendJsonString(out, Timestamp());
        out += ",\"level\":"; AppendJsonString(out, LevelName(level));
        out += ",\"service\":"; AppendJsonString(out, service);
        out += ",\"node_id\":"; AppendJsonString(out, node_id);
        out += ",\"pid\":" + std::to_string(pid);
        out += ",\"tid\":"; AppendJsonString(out, tid);
        out += ",\"file\":"; AppendJsonString(out, source);
        out += ",\"line\":" + std::to_string(line);
        if (event) { out += ",\"event\":"; AppendJsonString(out, event); }
        out += ",\"message\":"; AppendJsonString(out, message);
        if (!fields.empty()) out += ",\"data\":" + FieldsJson(fields);
        out += '}';
    }
    else
    {
        out = Timestamp() + " [" + LevelName(level) + "] [service=";
        AppendJsonString(out, service);
        out += " node_id="; AppendJsonString(out, node_id);
        out += " pid=" + std::to_string(pid) + " tid=" + tid + "] [";
        out.append(source);
        out += ':' + std::to_string(line) + "] ";
        if (event) { out += "[event="; out += event; out += "] "; }
        // Escape control characters without adding JSON's surrounding quotes.
        const auto begin = out.size();
        AppendJsonString(out, message);
        out.erase(begin, 1);
        out.pop_back();
        if (!fields.empty()) out += " data=" + FieldsJson(fields);
    }
    return out;
}
}
