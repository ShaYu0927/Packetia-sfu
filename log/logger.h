#ifndef LOGGER_H_
#define LOGGER_H_

#include <condition_variable>
#include <string>
#include <fstream>
#include <cassert>
#include <ctime>
#include <iostream>
#include <sstream>
#include <cstdio>
#include <mutex>
#include <variant>
#include <utility>
#include <memory>
#include <type_traits>
#include <atomic>
#include <deque>
#include <thread>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <stdexcept>
#include "ThreadManager.h"

#ifdef _WIN32
#include <process.h>
#else
#include <unistd.h>
#endif



inline std::string GetFileName(const std::string& filepath) 
{
    size_t pos = filepath.find_last_of("/\\");
    if (pos == std::string::npos) return filepath;
    return filepath.substr(pos + 1);
}


template<typename T>
std::string ToString(const T& val) 
{
    std::ostringstream oss;
    oss << val;
    return oss.str();
}


template<typename T>
void AppendToStream(std::ostringstream& oss, const T& val) 
{
    oss << val;
}

template<typename T, typename... Args>
void AppendToStream(std::ostringstream& oss, const T& val, const Args&... args) 
{
    oss << val;
    ((oss << ' ' << args), ...);
}

#define LOG_LEVEL_TRACE 0
#define LOG_LEVEL_DEBUG 1
#define LOG_LEVEL_INFO  2
#define LOG_LEVEL_ERR   3

#ifndef LOGGER_MIN_LEVEL
#define LOGGER_MIN_LEVEL LOG_LEVEL_INFO   // 默认 INFO
#endif



class ILogger 
{
public:
    enum Level { TRACE=0, DEBUG=1, INFO=2, ERR=3 };
    virtual ~ILogger() = default;

    virtual void set_min_level(int level) = 0;
    virtual int  min_level() const = 0;

    virtual void log(const char* file, int line, int level, std::string&& msg) = 0;
    virtual void flush() = 0;
};


class BaseLogger : public ILogger {
protected:
    std::atomic<int> minlevel_{LOG_LEVEL_INFO};

    static const char* level_str(int level) 
    {
        switch (level) 
        {
            case TRACE: return "TRACE";
            case DEBUG: return "DEBUG";
            case INFO:  return "INFO";
            case ERR:   return "ERROR";
            default:    return "UNKNOWN";
        }
    }

    static std::string now_str() 
    {
        const auto now = std::chrono::system_clock::now();
        const auto millis = std::chrono::duration_cast<std::chrono::milliseconds>(now.time_since_epoch()).count() % 1000;
        time_t sectime = std::chrono::system_clock::to_time_t(now);
        tm tmtime;
    #ifdef _WIN32
        localtime_s(&tmtime, &sectime);
    #else
        localtime_r(&sectime, &tmtime);
    #endif
        char buf[1024];
        snprintf(buf, sizeof(buf), "%04d-%02d-%02d %02d:%02d:%02d.%03d",
            tmtime.tm_year + 1900, tmtime.tm_mon + 1, tmtime.tm_mday,
            tmtime.tm_hour, tmtime.tm_min, tmtime.tm_sec, static_cast<int>(millis));
        return buf;
    }

    static void Context(std::ostringstream& stream)
    {
#ifdef _WIN32
        stream << " [pid=" << _getpid();
#else
        stream << " [pid=" << getpid();
#endif
        stream << " tid=" << std::this_thread::get_id() << "]";
    }

    // 派生类实现：真正写到哪里
    virtual void sink(std::string&& line) = 0;

public:
    void set_min_level(int level) override { minlevel_.store(level, std::memory_order_relaxed); }
    int  min_level() const override { return minlevel_.load(std::memory_order_relaxed); }

    template<typename... Args>
    void Write(const char* file, int line, int level, const Args&... args) 
    {
        if (level < min_level()) return;

        std::ostringstream oss;
        oss << now_str();
        Context(oss);
        oss << " [" << level_str(level) << "]: ["
            << GetFileName(file) << ":" << line << "] ";
        AppendToStream(oss, args...);

        sink(oss.str());
    }

    // ILogger::log：用于接收外部已格式化的字符串
    void log(const char* file, int line, int level, std::string&& msg) override 
    {
        if (level < min_level()) return;
        std::ostringstream oss;
        oss << now_str();
        Context(oss);
        oss << " [" << level_str(level) << "]: ["
            << GetFileName(file) << ":" << line << "] " << msg;
        sink(oss.str());
    }
};




struct LoggerOptions
{
    bool to_console = true;
    bool to_file = true;
    size_t max_queue_messages = 8192;
    size_t max_queue_bytes = 8 * 1024 * 1024;
    std::uintmax_t max_file_bytes = 16 * 1024 * 1024;
    size_t backup_files = 3;
    std::chrono::milliseconds flush_interval{200};
};

struct LoggerStats
{
    uint64_t accepted = 0;
    uint64_t written = 0;
    uint64_t dropped = 0;
    uint64_t write_failures = 0;
    size_t pending_messages = 0;
    size_t pending_bytes = 0;
};

class AsyncFileLogger : public BaseLogger
{
    struct Record { uint64_t sequence; std::string text; };
    const std::filesystem::path logfile_;
    const LoggerOptions options_;
    std::ofstream of_;
    std::uintmax_t file_bytes_ = 0;
    mutable std::mutex mtx_;
    std::condition_variable cv_;
    std::deque<Record> q_;
    LoggerStats stats_;
    uint64_t flushed_sequence_ = 0;
    uint64_t flush_target_ = 0;
    bool stopping_ = false;
    bool exited_ = false;
    std::string failure_;
    utils::ManagedThread worker_;

    void stop()
    {
        { std::lock_guard<std::mutex> lock(mtx_); stopping_ = true; }
        cv_.notify_all();
    }

    void rotate()
    {
        namespace fs = std::filesystem;
        of_.flush();
        of_.close();
        for (size_t index = options_.backup_files; index > 0; --index)
        {
            const fs::path destination = logfile_.string() + "." + std::to_string(index);
            const fs::path source = index == 1 ? logfile_
                : fs::path(logfile_.string() + "." + std::to_string(index - 1));
            if (fs::exists(destination))
            {
                if (!fs::is_regular_file(destination))
                    throw std::runtime_error("log backup is not a regular file: " + destination.string());
                fs::remove(destination);
            }
            if (fs::exists(source))
            {
                if (!fs::is_regular_file(source))
                    throw std::runtime_error("log source is not a regular file: " + source.string());
                fs::rename(source, destination);
            }
        }
        of_.open(logfile_, std::ios::out | std::ios::app | std::ios::binary);
        file_bytes_ = 0;
    }

    void write_line(const std::string& line)
    {
        if (options_.to_file)
        {
            const auto bytes = line.size() + 1;
            if (options_.max_file_bytes && file_bytes_ &&
                (file_bytes_ >= options_.max_file_bytes || bytes > options_.max_file_bytes - file_bytes_))
                rotate();
            of_ << line << '\n';
            file_bytes_ += bytes;
        }
        if (options_.to_console) std::cout << line << '\n';
    }

    void run_worker()
    {
        auto next_flush = std::chrono::steady_clock::now() + options_.flush_interval;
        uint64_t written = 0, reported_drops = 0;
        bool dirty = false;
        for (;;)
        {
            std::deque<Record> local;
            bool stopping, requested;
            uint64_t dropped;
            {
                std::unique_lock<std::mutex> lock(mtx_);
                cv_.wait_until(lock, next_flush, [&] {
                    return stopping_ || !q_.empty() || flush_target_ > flushed_sequence_ || stats_.dropped > reported_drops;
                });
                local.swap(q_);
                stopping = stopping_;
                requested = flush_target_ > flushed_sequence_;
                dropped = stats_.dropped;
            }
            size_t bytes = 0;
            for (const auto& record : local)
            {
                write_line(record.text);
                written = record.sequence;
                bytes += record.text.size() + 1;
                dirty = true;
            }
            if (dropped > reported_drops)
            {
                write_line(now_str() + " [LOGGER] dropped " + std::to_string(dropped - reported_drops) +
                           " log records (queue capacity or shutdown)");
                reported_drops = dropped;
                dirty = true;
            }
            // The console stays live; file buffers are flushed periodically or on demand.
            if (dirty && options_.to_console) std::cout.flush();
            const auto now = std::chrono::steady_clock::now();
            const bool flush_now = stopping || requested || now >= next_flush || !options_.to_file;
            if (flush_now)
            {
                if (dirty && options_.to_file) of_.flush();
                dirty = false;
                next_flush = now + options_.flush_interval;
            }
            const auto completed = local.size();
            local.clear();
            {
                std::lock_guard<std::mutex> lock(mtx_);
                stats_.written = written;
                stats_.pending_messages -= completed;
                stats_.pending_bytes -= bytes;
                if (flush_now) flushed_sequence_ = written;
                if (stopping && q_.empty()) exited_ = true;
            }
            cv_.notify_all();
            if (stopping) return;
        }
    }

    void record_failure(const char* message)
    {
        {
            std::lock_guard<std::mutex> lock(mtx_);
            failure_ = message;
            ++stats_.write_failures;
            stopping_ = exited_ = true;
            q_.clear();
            stats_.pending_messages = stats_.pending_bytes = 0;
        }
        cv_.notify_all();
        std::fprintf(stderr, "Packetia logger failed (%s): %s\n", logfile_.string().c_str(), message);
    }

    void worker_loop()
    {
        try { run_worker(); }
        catch (const std::exception& error) { record_failure(error.what()); throw; }
        catch (...) { record_failure("unknown logging exception"); throw; }
    }

protected:
    void sink(std::string&& line) override
    {
        {
            std::lock_guard<std::mutex> lock(mtx_);
            // Include the worker's current batch in the budget. Producers never wait on disk I/O.
            if (stopping_ || stats_.pending_messages >= options_.max_queue_messages ||
                line.size() >= options_.max_queue_bytes ||
                line.size() + 1 > options_.max_queue_bytes - stats_.pending_bytes)
                ++stats_.dropped;
            else
            {
                const auto bytes = line.size() + 1;
                q_.push_back({stats_.accepted + 1, std::move(line)});
                ++stats_.accepted;
                ++stats_.pending_messages;
                stats_.pending_bytes += bytes;
            }
        }
        cv_.notify_one();
    }

public:
    AsyncFileLogger(const std::string& logfile, bool to_console = true)
        : AsyncFileLogger(logfile, [to_console] { LoggerOptions options; options.to_console = to_console; return options; }()) {}

    AsyncFileLogger(const std::string& logfile, LoggerOptions options)
        : logfile_(logfile), options_(options)
    {
        if ((!options_.to_console && !options_.to_file) || !options_.max_queue_messages || !options_.max_queue_bytes ||
            options_.flush_interval.count() <= 0 ||
            (options_.max_file_bytes && (!options_.backup_files || options_.backup_files > 64)))
            throw std::invalid_argument("invalid logger options");
        if (options_.to_file)
        {
            try
            {
                const auto parent = logfile_.parent_path();
                if (!parent.empty()) std::filesystem::create_directories(parent);
                of_.exceptions(std::ios::failbit | std::ios::badbit);
                of_.open(logfile_, std::ios::out | std::ios::app | std::ios::binary);
                file_bytes_ = std::filesystem::file_size(logfile_);
            }
            catch (const std::exception& error)
            {
                throw std::runtime_error("cannot open log file " + logfile_.string() + ": " + error.what());
            }
        }
        worker_ = utils::ThreadManager::Instance().Start(
            {"logging", "logger", utils::ThreadModel::Logging},
            [this] { worker_loop(); }, [this] { stop(); });
    }

    ~AsyncFileLogger() override
    {
        worker_.RequestStop();
        worker_.Join();
    }

    LoggerStats stats() const
    {
        std::lock_guard<std::mutex> lock(mtx_);
        return stats_;
    }

    void flush() override
    {
        if (worker_.GetId() == std::this_thread::get_id())
            throw std::logic_error("logger worker cannot flush itself");
        std::unique_lock<std::mutex> lock(mtx_);
        const auto target = stats_.accepted;
        if (target > flush_target_) flush_target_ = target;
        cv_.notify_one();
        cv_.wait(lock, [&] { return flushed_sequence_ >= target || exited_; });
        if (!failure_.empty()) throw std::runtime_error("logger write failed: " + failure_);
    }
};



inline AsyncFileLogger& GlobalLogger()
{
    static AsyncFileLogger logger([] {
        const char* path = std::getenv("PACKETIA_LOG_FILE");
        return path ? std::string(path) : std::string("app.log");
    }(), [] {
        LoggerOptions options;
        if (const char* raw = std::getenv("PACKETIA_LOG_MODE"))
        {
            const std::string mode(raw);
            if (mode != "console" && mode != "file" && mode != "both")
                throw std::invalid_argument("PACKETIA_LOG_MODE expects console, file or both");
            options.to_file = mode != "console";
            options.to_console = mode != "file";
        }
        return options;
    }());
    return logger;
}

#if LOGGER_MIN_LEVEL <= LOG_LEVEL_DEBUG
#define LOG_DEBUG(...) GlobalLogger().Write(__FILE__, __LINE__, BaseLogger::DEBUG, __VA_ARGS__)
#else
#define LOG_DEBUG(...) ((void)0)
#endif

#if LOGGER_MIN_LEVEL <= LOG_LEVEL_INFO
#define LOG_INFO(...)  GlobalLogger().Write(__FILE__, __LINE__, BaseLogger::INFO,  __VA_ARGS__)
#else
#define LOG_INFO(...)  ((void)0)
#endif

#define LOG_ERROR(...) GlobalLogger().Write(__FILE__, __LINE__, BaseLogger::ERR, __VA_ARGS__)


#endif // LOGGER_H_

