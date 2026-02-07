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
    oss << ToString(val);
}

template<typename T, typename... Args>
void AppendToStream(std::ostringstream& oss, const T& val, const Args&... args) 
{
    oss << ToString(val) << " ";
    AppendToStream(oss, args...);
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
            case DEBUG: return "DEBUG";
            case INFO:  return "INFO";
            case ERR:   return "ERROR";
            default:    return "UNKNOWN";
        }
    }

    static std::string now_str() 
    {
        time_t sectime = time(nullptr);
        tm tmtime;
    #ifdef _WIN32
        localtime_s(&tmtime, &sectime);
    #else
        localtime_r(&sectime, &tmtime);
    #endif
        char buf[20];
        snprintf(buf, sizeof(buf), "%04d-%02d-%02d %02d:%02d:%02d",
            tmtime.tm_year + 1900, tmtime.tm_mon + 1, tmtime.tm_mday,
            tmtime.tm_hour, tmtime.tm_min, tmtime.tm_sec);
        return buf;
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
        oss << now_str() << " [" << level_str(level) << "]: ["
            << GetFileName(file) << ":" << line << "] ";
        AppendToStream(oss, args...);

        sink(oss.str());
    }

    // ILogger::log：用于接收外部已格式化的字符串
    void log(const char* file, int line, int level, std::string&& msg) override 
    {
        if (level < min_level()) return;
        std::ostringstream oss;
        oss << now_str() << " [" << level_str(level) << "]: ["
            << GetFileName(file) << ":" << line << "] " << msg;
        sink(oss.str());
    }
};




class AsyncFileLogger : public BaseLogger 
{
    std::ofstream of_;
    std::mutex mtx_;
    std::condition_variable cv_;
    std::deque<std::string> q_;
    std::thread worker_;
    std::atomic<bool> stop_{false};
    size_t max_queue_ = 100000;
    bool to_console_ = true;

    void worker_loop() 
    {
        std::unique_lock<std::mutex> lk(mtx_);
        while (!stop_.load() || !q_.empty()) 
        {
            cv_.wait(lk, [&]{ return stop_.load() || !q_.empty(); });

            std::deque<std::string> local;
            local.swap(q_);
            lk.unlock();

            for (auto& s : local) 
            {
                of_ << s << '\n';
                if (to_console_) std::cout << s << '\n';
            }
            of_.flush();

            lk.lock();
        }
    }

protected:
    void sink(std::string&& line) override 
    {
        {
            std::lock_guard<std::mutex> lk(mtx_);
            if (q_.size() >= max_queue_) q_.pop_front(); // 满了丢最旧
            q_.push_back(std::move(line));
        }
        cv_.notify_one();
    }

public:
    AsyncFileLogger(const std::string& logfile, bool to_console=true)
        : to_console_(to_console)
    {
        of_.open(logfile.c_str(), std::ios_base::out | std::ios_base::app);
        assert(of_.is_open());
        worker_ = std::thread([this]{ worker_loop(); });
    }

    ~AsyncFileLogger() override 
    {
        stop_.store(true);
        cv_.notify_all();
        if (worker_.joinable()) worker_.join();
        if (of_.is_open()) of_.close();
    }

    void flush() override 
    {
        std::unique_lock<std::mutex> lk(mtx_);
        cv_.wait_for(lk, std::chrono::milliseconds(200), [&]{ return q_.empty(); });
        of_.flush();
    }
};



inline BaseLogger& GlobalLogger() 
{
    static AsyncFileLogger logger("app.log", /*to_console=*/true);
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


