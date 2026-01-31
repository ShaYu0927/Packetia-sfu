#ifndef _EVENTLOOP_H_
#define _EVENTLOOP_H_

#include <functional>
#include <memory>
#include <vector>

#include "TaskScheduler.h"
#include "EpollTaskScheduler.h"
#include "TimeUtil.h"
#include "RingBuffer.h"
#include "Channel.h"

class Channel;
class EpollTaskScheduler;
class Pip;
class Timer;

#define TASK_SCHEDULER_PRIORITY_LOW       0
#define TASK_SCHEDULER_PRIORITY_NORMAL    1
#define TASK_SCHEDULER_PRIORITYO_HIGH     2 
#define TASK_SCHEDULER_PRIORITY_HIGHEST   3
#define TASK_SCHEDULER_PRIORITY_REALTIME  4

class EventLoop
{
public:
    EventLoop(const EventLoop&) = delete;
	EventLoop &operator = (const EventLoop&) = delete; 
    EventLoop(uint32_t num_threads =1);
    virtual ~EventLoop();

    std::shared_ptr<TaskScheduler> GetTaskScheduler();

    
    bool AddTriggerEvent(TriggerEvent callback);
    TimeId AddTimer(TimeEvent timerEvent, uint32_t msec);
    void RemoveTimer(TimeId timerId);
    void UpdateChannel(ChannelPtr channel);
    void RemoveChannel(ChannelPtr channel);

    void Loop();
    void Stop();

private:
    std::mutex mutex_;
	uint32_t num_threads_ = 1;
	uint32_t index_ = 1;
	std::vector<std::shared_ptr<TaskScheduler>> task_schedulers_;
	std::vector<std::shared_ptr<std::thread>> threads_;    
};

#endif