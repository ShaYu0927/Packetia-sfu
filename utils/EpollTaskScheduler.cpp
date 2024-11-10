#include "EpollTaskScheduler.h"

EpollTaskScheduler::EpollTaskScheduler(int id)
    :TaskScheduler(id)
{
    epollfd_ = epoll_create(1024);
    this->UpdateChannel(wakeup_channel_);
}

EpollTaskScheduler::~EpollTaskScheduler()
{
}

void EpollTaskScheduler::UpdateChannel(std::shared_ptr<Channel> channel)
{
    std::lock_guard<std::mutex> lock(mutex_);
    int fd = channel->GetSocket();
    if(channels_.find(fd) != channels_.end())
    {
        if(channel->IsNoneEvent())
        {
            Update(EPOLL_CTL_DEL, channel);
            channels_.erase(fd);
        }
        else
        {
            Update(EPOLL_CTL_MOD, channel);
        }
    }
    else
    {
        if(!channel->IsNoneEvent())
        {
            channels_.emplace(fd, channel);
            Update(EPOLL_CTL_ADD, channel);
        }
    }
}

void EpollTaskScheduler::RemoveChannel(std::shared_ptr<Channel> &channel)
{
    std::lock_guard<std::mutex> lock(mutex_);
    int fd = channel->GetSocket();

    if(channels_.find(fd) != channels_.end())
    {
        Update(EPOLL_CTL_DEL, channel);
		channels_.erase(fd);
    }
}

bool EpollTaskScheduler::HandleEvent(int timeout)
{
    struct epoll_event events[512] = {0};
	int num_events = -1;

    num_events = epoll_wait(epollfd_, events, 512, timeout);

    if(num_events < 0)  
    {
		if(errno != EINTR) 
        {
			return false;
		}								
	}

    for(int i = 0;i < num_events;i++)
    {
        if(events[i].data.ptr) 
        {        
			((Channel *)events[i].data.ptr)->HandleEvent(events[i].events);
		}
    }
    return true;
}

void EpollTaskScheduler::Update(int operation, std::shared_ptr<Channel> &channel)
{
    struct epoll_event event = {0};

	if(operation != EPOLL_CTL_DEL) {
		event.data.ptr = channel.get();
		event.events = channel->GetEvents();
	}

	if(::epoll_ctl(epollfd_, operation, channel->GetSocket(), &event) < 0) 
    {

	}
}
