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
    timer->SetEventCallback(event);
    time_map_.emplace(timer_id, timer);
    event_.emplace(std::pair<int64_t, TimeId>(timeOut + msec, timer_id), std::move(timer));
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
    if(!time_map_.empty())
    {
        std::lock_guard<std::mutex> lock(mutex_);
        int64_t timePoint = GetTimeNow();
        while(!time_map_.empty() && event_.begin()->first.first<=timePoint)
        {
            auto iter = event_.begin()->first.second;
            bool flag = event_.begin()->second->event_callback_();
            if(flag == true)
            {
                event_.begin()->second->SetNextTimeout(timePoint);
                auto timerPtr = std::move(event_.begin()->second);
                event_.erase(event_.begin());
                event_.emplace(std::pair<int64_t, TimeId>(timerPtr->getNextTimeout(), iter), timerPtr);
            }
            else {
                event_.erase(event_.begin());
                time_map_.erase(iter);
            }
        }
    }
}

int64_t TimeQueue::GetTimeNow()
{
    auto time_point = std::chrono::system_clock::now();  // 获取当前时间点
    return std::chrono::duration_cast<std::chrono::milliseconds>(time_point.time_since_epoch()).count();  // 转换为自纪元以来的毫秒数
}