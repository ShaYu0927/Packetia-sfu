#include "TaskScheduler.h"

TaskScheduler::TaskScheduler(int id)
    :id_(id)
    ,is_shutdown_(false) 
    ,wakeup_pipe_(new Pip())
    ,trigger_events_(new RingBuffer<TriggerEvent>(kMaxTriggetEvents))
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

    if(wakeup_pipe_->Create())
    {
        wakeup_channel_.reset(new Channel(wakeup_pipe_->Read()));
        wakeup_channel_->EnableReading();
        wakeup_channel_->SetReadCallback([this]{this->Wake();});
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
    if(trigger_events_->Size() < kMaxTriggetEvents)
    {
        std::lock_guard<std::mutex> lock(mutex_);
        char event = kTriggetEvent;
        trigger_events_->Push(std::move(callback));
        wakeup_pipe_->Write(&event,1);
        return true;
    }
    return false;
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
