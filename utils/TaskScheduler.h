#ifndef _TASK_SCHEDULER_H_
#define _TASK_SCHEDULER_H_

#include "Channel.h"
#include "Pip.h"
#include "TimeUtil.h"
#include "RingBuffer.h"
#include <functional>


typedef std::function<void(void)> TriggerEvent;

class TaskScheduler
{
public:
    using Task = std::function<void()>;

    TaskScheduler(int id = 1);
    virtual ~TaskScheduler();

    void start();
    void stop();

    virtual void UpdateChannel(std::shared_ptr<Channel> channel) { };
	virtual void RemoveChannel(std::shared_ptr<Channel>& channel) { };
	virtual bool HandleEvent(int timeout) { return false; };

    int GetId() const { return id_; }
    bool IsStopped() const { return shutdown_.load(); }
    bool IsStarted() const { return started_.load(); }


    TimeId AddTimer(TimeEvent timerEvent, uint32_t msec);
    void RemoveTimer(TimeId timerId);
    bool AddTriggerEvent(TriggerEvent callback);

    bool Post(Task task);

protected:
    void Wakeup();
    void DrainWakeupPipe();
    void HandlePendingTasks();
protected:
    void Wake();
	void HandleTriggerEvent();

    int id_ = 0;                                                //调度器的唯一 ID
    std::atomic_bool         is_shutdown_;
    std::unique_ptr<Pip>     wakeup_pipe_;                      // 唤醒管道，用于通知事件
    std::shared_ptr<Channel> wakeup_channel_;                   // 唤醒通道，用于事件处理。
    std::unique_ptr<RingBuffer<TriggerEvent>> trigger_events_;  //触发事件的环形缓冲区。

    std::atomic<bool> started_{false};
    std::atomic<bool> shutdown_{false};

    std::mutex task_mutex_;
    std::vector<Task> pending_tasks_;


    std::mutex mutex_;
	TimeQueue  timer_queue_;                                       //定时器队列，用于管理定时事件。


    static const char kTriggetEvent = 1;
	static const char kTimerEvent = 2;
	static const int  kMaxTriggetEvents = 50000;
};


#endif
