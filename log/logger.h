#ifndef LOGGER_H_
#define LOGGER_H_

#include <string>
#include <fstream>
#include <cassert>
#include <ctime>
#include <iostream>
#include <sstream>
#include <cstdio>
#include <mutex>


// 提取文件名，不带路径
inline std::string GetFileName(const std::string& filepath) 
{
    size_t pos = filepath.find_last_of("/\\");
    if (pos == std::string::npos) return filepath;
    return filepath.substr(pos + 1);
}

// 辅助把任意类型转字符串
template<typename T>
std::string ToString(const T& val) 
{
    std::ostringstream oss;
    oss << val;
    return oss.str();
}

// 辅助把多个参数拼成一个字符串，中间空格分隔
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

#include <variant>
#include <utility>
#include <memory>
#include <type_traits>


template <typename T>
struct is_shared_ptr : std::false_type {};

template <typename T>
struct is_shared_ptr<std::shared_ptr<T>> : std::true_type {};

template <typename T>
inline constexpr bool is_shared_ptr_v = is_shared_ptr<T>::value;

template <class... Ts>
std::string ToString(const std::variant<Ts...>& v) {
    std::ostringstream oss;
    std::visit([&](auto&& x) {
        using X = std::decay_t<decltype(x)>;
        if constexpr (std::is_same_v<X, std::monostate>) {
            oss << "monostate";
        } else if constexpr (std::is_same_v<X, std::pair<unsigned char*, size_t>> ||
                             std::is_same_v<X, std::pair<uint8_t*, size_t>>) {
            oss << "bytes(ptr=" << (void*)x.first << ", len=" << x.second << ")";
        } else if constexpr (is_shared_ptr_v<X>) {
            // 不依赖 T 的定义：只打印地址/引用计数
            oss << "shared_ptr(ptr=" << (void*)x.get() << ", use_count=" << x.use_count() << ")";
        } else {
            oss << "<variant-alternative>";
        }
    }, v);
    return oss.str();
}



class Logger 
{
private:
    std::ofstream of_;
    int minlevel_;
    std::mutex mtx_; 

public:
    enum Level 
    {
        TRACE = LOG_LEVEL_TRACE,
        DEBUG = LOG_LEVEL_DEBUG,
        INFO  = LOG_LEVEL_INFO,
        ERR   = LOG_LEVEL_ERR
    };

    Logger(const int level, const std::string& logfile) : minlevel_(level) 
    {
        this->of_.open(logfile.c_str(), std::ios_base::out | std::ios_base::app);
        assert(this->of_.is_open() && "Failed to open log file");
    }

    ~Logger() 
    {
        if (this->of_.is_open()) 
        {
            this->of_.close();
        }
    }

    template<typename... Args>
    void Write(const std::string& codefile, int codeline, int level, const Args&... args) 
    {
        if (level < minlevel_) return;

        std::lock_guard<std::mutex> lk(mtx_);

        time_t sectime = time(NULL);
        tm tmtime;

#ifdef _WIN32
#if _MSC_VER < 1600
        tmtime = *localtime(&sectime);
#else
        localtime_s(&tmtime, &sectime);
#endif
#else
        localtime_r(&sectime, &tmtime);
#endif

        char time_buf[20];
        snprintf(time_buf, sizeof(time_buf), "%04d-%02d-%02d %02d:%02d:%02d",
            tmtime.tm_year + 1900, tmtime.tm_mon + 1, tmtime.tm_mday,
            tmtime.tm_hour, tmtime.tm_min, tmtime.tm_sec);

        const char* level_str = nullptr;
        switch (level) {
        case DEBUG: level_str = "DEBUG"; break;
        case INFO:  level_str = "INFO";  break;
        case ERR:   level_str = "ERROR"; break;
        default:    level_str = "UNKNOWN"; break;
        }

        std::ostringstream oss;
        oss << time_buf << " [" << level_str << "]: [" << GetFileName(codefile) << ":" << codeline << "] ";

        AppendToStream(oss, args...);

        std::string log_line = oss.str();

        of_ << log_line << std::endl;
        std::cout << log_line << std::endl;
    }
};

// 全局Logger单例（你可以根据需要改成局部或者更灵活）
inline Logger& GlobalLogger() 
{
    static Logger logger(LOGGER_MIN_LEVEL, "app.log");
    return logger;
}

// 宏定义
#if LOGGER_MIN_LEVEL <= LOG_LEVEL_DEBUG
#define LOG_DEBUG(...) GlobalLogger().Write(__FILE__, __LINE__, Logger::DEBUG, __VA_ARGS__)
#else
#define LOG_DEBUG(...) ((void)0)
#endif

#if LOGGER_MIN_LEVEL <= LOG_LEVEL_INFO
#define LOG_INFO(...)  GlobalLogger().Write(__FILE__, __LINE__, Logger::INFO,  __VA_ARGS__)
#else
#define LOG_INFO(...)  ((void)0)
#endif

#define LOG_ERROR(...) GlobalLogger().Write(__FILE__, __LINE__, Logger::ERR, __VA_ARGS__)

#endif // LOGGER_H_


