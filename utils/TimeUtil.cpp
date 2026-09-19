//
// Created by roots on 2024/9/12.
//

#include "TimeUtil.h"


TimeId TimeQueue::AddTimer(const TimeEvent& event, uint32_t msec)
{
    std::lock_guard<std::mutex> lock(mutex_);
    int64_t timeOut = GetTimeNow();
    TimeId timer_id = ++last_timer_id_;

    auto timer = std::make_shared<TimeUtil>(event, msec);
    timer->SetNextTimeout(timeOut);
    time_map_.emplace(timer_id, timer);
    event_.emplace(std::make_pair(timer->getNextTimeout(), timer_id), timer);
    return timer_id;
}
void TimeQueue::RemoveTimer(TimeId id)
{
    std::lock_guard<std::mutex> lock(mutex_);
    auto iter = time_map_.find(id);
    if (iter != time_map_.end()) {
        int64_t timeout = iter->second->getNextTimeout();
        event_.erase(std::pair<int64_t, TimeId>(timeout, id));
        time_map_.erase(id);
    }
}

int64_t TimeQueue::GetTimeRemain()
{
    std::lock_guard<std::mutex> lock(mutex_);
    if(event_.empty()) {
        return -1;
    }
    int64_t msec = event_.begin()->first.first - GetTimeNow();
    if (msec < 0) {
        msec = 0;
    }
    return msec;
}
void TimeQueue::HandleTimerEvent()
{
    const auto deadline = GetTimeNow();
    for (;;)
    {
        TimeId id;
        std::shared_ptr<TimeUtil> timer;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            if (event_.empty() || event_.begin()->first.first > deadline) return;
            id = event_.begin()->first.second;
            timer = event_.begin()->second;
            event_.erase(event_.begin());
        }

        // Callbacks may add/remove timers, including this timer. Keep the
        // timer alive without holding the queue lock across application code.
        bool repeat = false;
        try { repeat = timer->triggerEventCallback(); }
        catch (...)
        {
            RemoveTimer(id);
            throw;
        }
        std::lock_guard<std::mutex> lock(mutex_);
        auto it = time_map_.find(id);
        if (it == time_map_.end() || it->second != timer) continue;
        if (repeat)
        {
            timer->SetNextTimeout(GetTimeNow());
            event_.emplace(std::make_pair(timer->getNextTimeout(), id), timer);
        }
        else time_map_.erase(it);
    }
}

int64_t TimeQueue::GetTimeNow()
{
    auto time_point = std::chrono::steady_clock::now();
    return std::chrono::duration_cast<std::chrono::milliseconds>(time_point.time_since_epoch()).count();  // 转换为自纪元以来的毫秒数
}
