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

void EventLoop::Loop()
{
    std::lock_guard<std::mutex> lock(mutex_);
    if(!task_schedulers_.empty())
    {
        return ;
    }

    for (uint32_t i = 0; i < num_threads_; ++i) {
        std::shared_ptr<TaskScheduler> task_scheduler =
            std::make_shared<EpollTaskScheduler>(scheduler_id_seed_++);
        task_schedulers_.push_back(task_scheduler);

        std::shared_ptr<std::thread> thread =
            std::make_shared<std::thread>([task_scheduler]() {
                while (!task_scheduler->IsStopped()) 
				{
                    task_scheduler->HandleEvent(100);
                }
            });

        threads_.push_back(thread);
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
}

void EventLoop::Stop()
{
    std::lock_guard<std::mutex> lock(mutex_);
    if (!started_) 
	{
        return;
    }

    for (auto& iter : task_schedulers_) 
	{
        iter->stop();
    }

    for (auto& iter : threads_) 
	{
        if (iter && iter->joinable()) {
            iter->join();
        }
    }

    task_schedulers_.clear();
    threads_.clear();
    started_ = false;
}
