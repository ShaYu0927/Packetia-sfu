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
    {
        std::lock_guard<std::mutex> lock(task_mutex_);
        shutdown_.store(true);
    }
    char event = kTriggetEvent;
    if (wakeup_pipe_) wakeup_pipe_->Write(&event, 1);
}

void TaskScheduler::Run()
{
    current_ = this;
    {
        std::lock_guard<std::mutex> lock(task_mutex_);
        started_.store(true);
    }
    for (;;)
    {
        HandleEvent(100);
        HandlePendingTasks();
        std::lock_guard<std::mutex> lock(task_mutex_);
        if (shutdown_ && pending_tasks_.empty())
        {
            started_.store(false);
            break;
        }
    }
    current_ = nullptr;
}

void TaskScheduler::Invoke(Task task)
{
    if (IsCurrentThread()) { task(); return; }
    auto work = std::make_shared<std::packaged_task<void()>>(std::move(task));
    auto done = work->get_future();
    bool queued = false;
    {
        std::lock_guard<std::mutex> lock(task_mutex_);
        if (started_)
        {
            // Cleanup must also be accepted while Run is draining on stop.
            pending_tasks_.push_back([work] { (*work)(); });
            queued = true;
        }
    }
    if (queued)
    {
        char event = kTriggetEvent;
        if (wakeup_pipe_) wakeup_pipe_->Write(&event, 1);
    }
    else { (*work)(); }
    done.get();
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
    return Post(std::move(callback));
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
        if (shutdown_ || !wakeup_pipe_ || pending_tasks_.size() >= kMaxTriggetEvents)
        {
            return false;
        }
        pending_tasks_.push_back(std::move(task));
    }

    char event = kTriggetEvent;
    wakeup_pipe_->Write(&event, 1);
    // A full nonblocking wakeup pipe is already readable. The task was
    // accepted and must not be reported as failed (callers might retry it).
    return true;
}
