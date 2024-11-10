#include "SelectTaskScheduler.h"

SelectTaskScheduler::SelectTaskScheduler(int id)
    :TaskScheduler(id)
{
    FD_ZERO(&fd_read_backup_);
    FD_ZERO(&fd_write_backup_);
    FD_ZERO(&fd_exp_backup_);
}

SelectTaskScheduler::~SelectTaskScheduler()
{
}

void SelectTaskScheduler::UpdateChannel(std::shared_ptr<Channel> channel)
{
    std::lock_guard<std::mutex> lock(mutex_);
    int socket = channel->GetSocket();
    if(channels_.find(socket) != channels_.end())
    {
        if(channel->IsNoneEvent())
        {
            is_fd_write_reset_ = true;
            is_fd_read_reset_  = true;
            is_fd_exp_reset_   = true;
            channels_.erase(socket);
        }
        else
        {
            is_fd_write_reset_ = true;
        }
    }
    else
    {
        if(!channel->IsNoneEvent())
        {
            channels_.emplace(socket,channel);
            is_fd_read_reset_ = true;
			is_fd_write_reset_ = true;
			is_fd_exp_reset_ = true;
        }
    }
}

void SelectTaskScheduler::RemoveChannel(std::shared_ptr<Channel> channel)
{
    std::lock_guard<std::mutex> lock(mutex_);

    int fd = channel->GetSocket();
    if(channels_.find(fd) != channels_.end())
    {
        is_fd_read_reset_ = true;
		is_fd_write_reset_ = true;
		is_fd_exp_reset_ = true;
		channels_.erase(fd);
    }
}

bool SelectTaskScheduler::HandleEvent(int timeout)
{
    if(channels_.empty())
    {
        if(timeout < 0)
        {
            timeout = 10;
        }
        Time::sleep(timeout);
        return true;
    }

    fd_set fd_read;
    fd_set fd_write;
    fd_set fd_exp;

    FD_ZERO(&fd_read);
	FD_ZERO(&fd_write);
	FD_ZERO(&fd_exp);
	bool fd_read_reset = false;
	bool fd_write_reset = false;
	bool fd_exp_reset = false;

    if(is_fd_read_reset_ || is_fd_write_reset_ || is_fd_exp_reset_ )
    {
        if(is_fd_exp_reset_)
        {
            maxfd_ = 0;
        }
        std::lock_guard<std::mutex> lock(mutex_);
        for(auto iter : channels_)
        {
            int events = iter.second->GetEvents();
			SOCKET fd = iter.second->GetSocket();

			if (is_fd_read_reset_ && (events&EVENT_IN)) //如果需要重置可读事件，并且该事件标记为 EVENT_IN
            {
				FD_SET(fd, &fd_read);
			}

			if(is_fd_write_reset_ && (events&EVENT_OUT)) //如果需要重置可写事件，并且该事件标记为 EVENT_OUT
            {
				FD_SET(fd, &fd_write);
			}

			if(is_fd_exp_reset_) 
            {
				FD_SET(fd, &fd_exp);
				if(fd > maxfd_) 
                {
					maxfd_ = fd;
				}
			}		
        }
        fd_read_reset = is_fd_read_reset_;
		fd_write_reset = is_fd_write_reset_;
		fd_exp_reset = is_fd_exp_reset_;
		is_fd_read_reset_ = false;
		is_fd_write_reset_ = false;
		is_fd_exp_reset_ = false;
    }
    if(fd_read_reset)
    {
        FD_ZERO(&fd_read_backup_);
        memcpy(&fd_read_backup_, &fd_read, sizeof(fd_set));
    }
    else
    {
        memcpy(&fd_read, &fd_read_backup_, sizeof(fd_set));
    }

    if(fd_write_reset)
    {
        FD_ZERO(&fd_write_backup_);
        memcpy(&fd_write_backup_,&fd_write,sizeof(fd_set));

    }
    else
    {
        memcpy(&fd_write,&fd_write_backup_,sizeof(fd_set));
    }

    if(fd_exp_reset) 
    {
		FD_ZERO(&fd_exp_backup_);
		memcpy(&fd_exp_backup_, &fd_exp, sizeof(fd_set));
	}
	else 
    {
		memcpy(&fd_exp, &fd_exp_backup_, sizeof(fd_set));
	}

    if(timeout <= 0)
    {
        timeout = 10;
    }

    struct timeval tv = {timeout/1000, timeout%1000*1000};
    int ret = select((int)maxfd_,&fd_read,&fd_write,&fd_exp,&tv);
    if(ret < 0)
    {
#if defined(__linux) || defined(__linux__) 
	if(errno == EINTR) {
		return true;
	}					
#endif 
		return false;
    }

    /*
        在 select 返回的文件描述符集合中检查哪些文件描述符发生了事件，
        并将事件添加到 event_list 中。
    
     */
    std::forward_list<std::pair<std::shared_ptr<Channel>,int>> event_lists;
    if(ret > 0)
    {
        std::lock_guard<std::mutex> lock(mutex_);
        for(auto iter : channels_)
        {
            int events = 0;
            int socket = iter.second->GetSocket();

            if(FD_ISSET(socket, &fd_read))
            {
                events |= EVENT_IN;
            }

            if (FD_ISSET(socket, &fd_write)) 
            {
				events |= EVENT_OUT;
			}

            if(FD_ISSET(socket, &fd_exp))
            {
                events |= (EVENT_HUP);
            }

            if(events != 0)
            {
                event_lists.emplace_front(iter.second, events);
            }
        }
    }

    //事件处理回调
    for(auto iter : event_lists)
    {
        iter.first->HandleEvent(iter.second);
    }
    return false;
}
