#include "TaskScheduler.h"
#include <signal.h>

TaskScheduler::TaskScheduler(int id)
     : id_(id)
    , is_shutdown_(false)
    , shutdown_(false)
    , started_(false)
    , wakeup_pipe_(new Pip())
    , trigger_events_(new RingBuffer<TriggerEvent>(kMaxTriggetEvents))
{
    static std::once_flag flag;
    std::call_once(flag,[]{
#if defined(WIN32) || defined(_WIN32)
    WSADATA wsa_data;
    if (WSAStartup(MAKEWORD(2, 2), &wsa_data)) {
			WSACleanup();
		}
#endif             
    });

    if (wakeup_pipe_ && wakeup_pipe_->Create()) 
    {
        wakeup_channel_.reset(new Channel(wakeup_pipe_->ReadFd()));
        wakeup_channel_->EnableReading();
        wakeup_channel_->SetReadCallback([this] {
            this->DrainWakeupPipe();
            this->HandleTriggerEvent();
            this->HandlePendingTasks();
        });
    }
}

TaskScheduler::~TaskScheduler()
{
}

void TaskScheduler::start()
{
    signal(SIGPIPE, SIG_IGN);
	signal(SIGQUIT, SIG_IGN);
	signal(SIGUSR1, SIG_IGN);
	signal(SIGTERM, SIG_IGN);
	signal(SIGKILL, SIG_IGN);
}

void TaskScheduler::stop()
{
    is_shutdown_ = true;
    char event = kTriggetEvent;
    wakeup_pipe_->Write(&event,1);
    shutdown_.store(true);
    started_.store(false);
}

TimeId TaskScheduler::AddTimer(TimeEvent timerEvent, uint32_t msec)
{
    TimeId timeId = timer_queue_.AddTimer(timerEvent,msec);
    return timeId;
}

void TaskScheduler::RemoveTimer(TimeId timerId)
{
    timer_queue_.RemoveTimer(timerId);
}

bool TaskScheduler::AddTriggerEvent(TriggerEvent callback)
{
    if (!callback || !trigger_events_)
    {
        return false;
    }

    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (trigger_events_->Size() >= kMaxTriggetEvents ||
            !trigger_events_->Push(std::move(callback)))
        {
            return false;
        }
    }

    char event = kTriggetEvent;
    return wakeup_pipe_ && wakeup_pipe_->Write(&event, 1) == 1;
}

void TaskScheduler::Wake()
{
    char event[10] = {0};
    while(wakeup_pipe_->Read(event, 10) > 0);
}

void TaskScheduler::HandleTriggerEvent()
{
    do
    {
       TriggerEvent callback;
       if(trigger_events_->Pop(callback))
       {
            callback();
       }
    } while (trigger_events_->Size() > 0);
    
}

void TaskScheduler::HandlePendingTasks()
{
    std::vector<Task> tasks;

    {
        std::lock_guard<std::mutex> lock(task_mutex_);
        tasks.swap(pending_tasks_);
    }

    for (auto& task : tasks)
    {
        if (task)
        {
            task();
        }
    }
}

void TaskScheduler::DrainWakeupPipe()
{
    if (!wakeup_pipe_) return;

    char buf[128];
    while (wakeup_pipe_->Read(buf, sizeof(buf)) > 0)
    {
    }
}

bool TaskScheduler::Post(Task task)
{
    if (!task)
    {
        return false;
    }

    {
        std::lock_guard<std::mutex> lock(task_mutex_);
        if (pending_tasks_.size() >= kMaxTriggetEvents)
        {
            return false;
        }
        pending_tasks_.push_back(std::move(task));
    }

    char event = kTriggetEvent;
    return wakeup_pipe_ && wakeup_pipe_->Write(&event, 1) == 1;
}
