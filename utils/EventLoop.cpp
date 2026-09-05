#include "EventLoop.h"
#include "EpollTaskScheduler.h"


EventLoop::EventLoop(uint32_t num_threads)
    : scheduler_id_seed_(0),
      next_scheduler_index_(0)
{
    num_threads_ = 1;
	num_threads_ = (num_threads > 0) ? num_threads : 1;
}

EventLoop::~EventLoop()
{
	this->Stop();
}

std::shared_ptr<TaskScheduler> EventLoop::GetTaskScheduler()
{
    std::lock_guard<std::mutex> locker(mutex_);
    if (task_schedulers_.empty()) return {};

    auto ts = task_schedulers_[next_scheduler_index_ % task_schedulers_.size()];
    ++next_scheduler_index_;
    return ts;
}

bool EventLoop::AddTriggerEvent(TriggerEvent callback)
{
    std::lock_guard<std::mutex> locker(mutex_);
	if (task_schedulers_.size() > 0) 
	{
		return task_schedulers_[0]->AddTriggerEvent(callback);
	}
	return false;
}

TimeId EventLoop::AddTimer(TimeEvent timerEvent, uint32_t msec)
{
    std::lock_guard<std::mutex> locker(mutex_);
	if (task_schedulers_.size() > 0) 
    {
		return task_schedulers_[0]->AddTimer(timerEvent, msec);
	}
	return 0;
}

void EventLoop::RemoveTimer(TimeId timerId)
{
    std::lock_guard<std::mutex> locker(mutex_);
	if (task_schedulers_.size() > 0) 
	{
		task_schedulers_[0]->RemoveTimer(timerId);
	}	
}

void EventLoop::UpdateChannel(ChannelPtr channel)
{
    std::lock_guard<std::mutex> locker(mutex_);
    if (task_schedulers_.size() > 0) 
    {
		task_schedulers_[0]->UpdateChannel(channel);
	}
}

void EventLoop::RemoveChannel(ChannelPtr channel)
{
    std::lock_guard<std::mutex> locker(mutex_);
	if (task_schedulers_.size() > 0) 
    {
		task_schedulers_[0]->RemoveChannel(channel);
	}
}

bool EventLoop::Start()
{
    std::lock_guard<std::mutex> lock(mutex_);
    if(!task_schedulers_.empty())
    {
        return false;
    }

    for (uint32_t i = 0; i < num_threads_; ++i) 
    {
        std::shared_ptr<TaskScheduler> task_scheduler =
            std::make_shared<EpollTaskScheduler>(scheduler_id_seed_++);
        task_schedulers_.push_back(task_scheduler);

        std::shared_ptr<std::thread> thread =
            std::make_shared<std::thread>([task_scheduler]() {
                task_scheduler->Run();
            });

        threads_.push_back(thread);
        while (!task_scheduler->IsStarted()) std::this_thread::yield();
    }

    started_ = true;

    const int priority = TASK_SCHEDULER_PRIORITY_REALTIME;

    for (auto iter : threads_) 
	{
#if defined(__linux) || defined(__linux__) 

#elif defined(WIN32) || defined(_WIN32) 
		switch (priority) 
		{
		case TASK_SCHEDULER_PRIORITY_LOW:
			SetThreadPriority(iter->native_handle(), THREAD_PRIORITY_BELOW_NORMAL);
			break;
		case TASK_SCHEDULER_PRIORITY_NORMAL:
			SetThreadPriority(iter->native_handle(), THREAD_PRIORITY_NORMAL);
			break;
		case TASK_SCHEDULER_PRIORITYO_HIGH:
			SetThreadPriority(iter->native_handle(), THREAD_PRIORITY_ABOVE_NORMAL);
			break;
		case TASK_SCHEDULER_PRIORITY_HIGHEST:
			SetThreadPriority(iter->native_handle(), THREAD_PRIORITY_HIGHEST);
			break;
		case TASK_SCHEDULER_PRIORITY_REALTIME:
			SetThreadPriority(iter->native_handle(), THREAD_PRIORITY_TIME_CRITICAL);
			break;
		}
#endif
	}
    return true;
}

void EventLoop::Loop()
{
    while (started_) 
    {
        std::this_thread::sleep_for(std::chrono::seconds(1));
    }
}

void EventLoop::Stop()
{
    std::vector<std::shared_ptr<std::thread>> threads;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        for (auto& scheduler : task_schedulers_) scheduler->stop();
        started_ = false;
        // An I/O callback can request stop, but cannot join its own thread.
        // The owning thread/destructor subsequently completes the joins.
        for (auto& scheduler : task_schedulers_)
            if (scheduler->IsCurrentThread()) return;
    }
    // Serialize controlling callers without blocking an I/O callback that
    // merely requests stop while another caller is joining it.
    std::lock_guard<std::mutex> joining(join_mutex_);
    {
        std::lock_guard<std::mutex> lock(mutex_);
        threads.swap(threads_);
    }
    for (auto& thread : threads)
        if (thread && thread->joinable()) thread->join();
    std::lock_guard<std::mutex> lock(mutex_);
    // Connections and servers retain their old scheduler for final cleanup.
    // A subsequent Start creates new schedulers.
    task_schedulers_.clear();
}
