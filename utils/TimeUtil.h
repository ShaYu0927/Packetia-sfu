//
// Created by roots on 2024/9/12.
//

#ifndef FFMPEGAAC_TIME_H
#define FFMPEGAAC_TIME_H

#include <map>
#include <unordered_map>
#include <chrono>
#include <functional>
#include <cstdint>
#include <chrono>
#include <memory>
#include <mutex>
#include <thread>

using namespace std::chrono;
typedef uint32_t TimeId;
typedef std::function<bool(void)> TimeEvent;

class TimeUtil
{
public:
    TimeUtil(const TimeEvent& event, uint32_t msec)
    :event_callback_ (event), interval_(msec)
    {
        if (msec == 0) {
            interval_ = 1;
        }
    }

    ~TimeUtil() {}


    static void sleep(uint32_t msec)
    {
        std::this_thread::sleep_for(std::chrono::milliseconds(msec));
    }

    void SetEventCallback(const TimeEvent& event)
    {
        event_callback_ = event;
    }

    void Start(int64_t microseconds, bool is_reapte)
    {
        is_reapte_ = is_reapte;
        auto time_begin = std::chrono::high_resolution_clock::now();
        int64_t elapsed = 0;

        do
        {
            std::this_thread::sleep_for(std::chrono::microseconds(microseconds - elapsed));
            time_begin = std::chrono::high_resolution_clock::now();
            if(event_callback_)
            {
                event_callback_();
            }
            elapsed = std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::high_resolution_clock::now() - time_begin).count();
            if (elapsed < 0) {
                elapsed = 0;
            }
        } while(is_reapte_);
    }

    void Stop()
    {
        is_reapte_ = false;
    }

    
    void SetNextTimeout(int64_t time_point)
    {
        next_timeout_ = time_point + interval_;
    }

    int64_t getNextTimeout() const
    {
        return next_timeout_;
    }

    bool triggerEventCallback() 
    {
        return event_callback_();
    }
    TimeEvent event_callback_ = [] { return false; };
private:
    friend class TimerQueue;
    bool is_reapte_ = false;
    uint32_t interval_ = 0;
    int64_t next_timeout_ = 0;

};

class TimeQueue
{
public:
    TimeId AddTimer(const TimeEvent& event, uint32_t msec);
    void RemoveTimer(TimeId id);

    int64_t GetTimeRemain();
    void HandleTimerEvent();

private:
    friend class TimeUtil;
    int64_t GetTimeNow();


    std::mutex mutex_;
    std::unordered_map<TimeId, std::shared_ptr<TimeUtil>> time_map_;
    std::map<std::pair<int64_t, TimeId>,std::shared_ptr<TimeUtil>> event_;
    uint32_t last_timer_id_ = 0;

};

class Timestamp
{
public:
    using SteadyClock = std::chrono::steady_clock;
    using SystemClock = std::chrono::system_clock;

public:
    static uint64_t NowMs()
    {
        return std::chrono::duration_cast<std::chrono::milliseconds>(
                   SteadyClock::now().time_since_epoch())
            .count();
    }

    static uint64_t NowUs()
    {
        return std::chrono::duration_cast<std::chrono::microseconds>(
                   SteadyClock::now().time_since_epoch())
            .count();
    }

    static uint64_t WallNowMs()
    {
        return std::chrono::duration_cast<std::chrono::milliseconds>(
                   SystemClock::now().time_since_epoch())
            .count();
    }

    static uint64_t WallNowUs()
    {
        return std::chrono::duration_cast<std::chrono::microseconds>(
                   SystemClock::now().time_since_epoch())
            .count();
    }

    static uint64_t ElapsedMs(uint64_t begin_ms)
    {
        uint64_t now = NowMs();
        return (now >= begin_ms) ? (now - begin_ms) : 0;
    }

    static uint64_t ElapsedUs(uint64_t begin_us)
    {
        uint64_t now = NowUs();
        return (now >= begin_us) ? (now - begin_us) : 0;
    }
};

#endif //FFMPEGAAC_TIME_H
