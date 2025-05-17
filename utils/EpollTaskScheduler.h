#ifndef _EPOLL_TASK_SCHEDULER_H_
#define _EPOLL_TASK_SCHEDULER_H_


#include "TaskScheduler.h"
#include "logger.h"
#include <sys/epoll.h>
#include <errno.h>

class  EpollTaskScheduler : public TaskScheduler
{
public:
    EpollTaskScheduler(int id = 0);
    virtual ~EpollTaskScheduler();

    void UpdateChannel(std::shared_ptr<Channel> channel);
	void RemoveChannel(std::shared_ptr<Channel>& channel);

	// timeout: ms
	bool HandleEvent(int timeout);

private:
    void Update(int operation, std::shared_ptr<Channel>& channel);
    int epollfd_ = -1;

	std::mutex mutex_;
	std::unordered_map<SOCKET, std::shared_ptr<Channel>> channels_;

};


#endif