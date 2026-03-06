#include "EpollTaskScheduler.h"
#include <errno.h>


EpollTaskScheduler::EpollTaskScheduler(int id)
    :TaskScheduler(id)
{
    epollfd_ = epoll_create1(EPOLL_CLOEXEC);
    if (wakeup_channel_) 
    {
        UpdateChannel(wakeup_channel_);
    }
}

EpollTaskScheduler::~EpollTaskScheduler()
{
    if (epollfd_ >= 0) 
    {
        ::close(epollfd_);
        epollfd_ = -1;
    }
}

void EpollTaskScheduler::UpdateChannel(std::shared_ptr<Channel> channel)
{
    std::lock_guard<std::mutex> lock(channel_mutex_);
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
    std::lock_guard<std::mutex> lock(channel_mutex_);
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
            auto fd = ((Channel *)events[i].data.ptr)->GetSocket();
            uint32_t ev = events[i].events;
#if RTP_DEBUG
            LOG_INFO("Event on fd=" + std::to_string(fd) + " events=" + std::to_string(ev));
#endif
			((Channel *)events[i].data.ptr)->HandleEvent(events[i].events);
		}
    }
    return true;
}

void EpollTaskScheduler::Update(int operation, std::shared_ptr<Channel> &channel)
{
    struct epoll_event event = {0};

	if(operation != EPOLL_CTL_DEL) 
    {
		event.data.ptr = channel.get();
		event.events = channel->GetEvents();
	}

	if(::epoll_ctl(epollfd_, operation, channel->GetSocket(), &event) < 0) 
    {
        perror("epoll_ctl error");
	}
}
