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
        std::shared_ptr<Channel> channel;
        {
            std::lock_guard<std::mutex> lock(channel_mutex_);
            // epoll_event is packed on Linux; do not bind an unordered_map
            // key reference directly to its potentially unaligned member.
            const uint64_t token = events[i].data.u64;
            auto it = token_channels_.find(token);
            if (it != token_channels_.end()) channel = it->second.lock();
        }
        if (channel) channel->HandleEvent(events[i].events);
    }
    return true;
}

void EpollTaskScheduler::Update(int operation, std::shared_ptr<Channel> &channel)
{
    struct epoll_event event = {0};

    const int fd = channel->GetSocket();
    if (operation == EPOLL_CTL_ADD)
    {
        const auto token = next_token_++;
        channel_tokens_[fd] = token;
        token_channels_[token] = channel;
    }
    if (operation == EPOLL_CTL_DEL)
    {
        auto it = channel_tokens_.find(fd);
        if (it != channel_tokens_.end())
        {
            token_channels_.erase(it->second);
            channel_tokens_.erase(it);
        }
    }
	if(operation != EPOLL_CTL_DEL)
    {
		event.data.u64 = channel_tokens_.at(fd);
		event.events = channel->GetEvents();
	}

	if(::epoll_ctl(epollfd_, operation, channel->GetSocket(), &event) < 0) 
    {
        perror("epoll_ctl error");
	}
}
