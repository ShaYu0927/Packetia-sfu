#ifndef _EPOLL_TASK_SCHEDULER_H_
#define _EPOLL_TASK_SCHEDULER_H_


#include "TaskScheduler.h"

#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#else
  #include <sys/select.h>
  #include <sys/time.h>
  #include <unistd.h>
  #include <sys/epoll.h>
#endif

#include <unistd.h>     



class  EpollTaskScheduler : public TaskScheduler
{
public:
    EpollTaskScheduler(int id = 0);
    virtual ~EpollTaskScheduler();

    void UpdateChannel(std::shared_ptr<Channel> channel);
	void RemoveChannel(std::shared_ptr<Channel>& channel);
	bool HandleEvent(int timeout);

private:
    void Update(int operation, std::shared_ptr<Channel>& channel);
    int epollfd_ = -1;

	std::mutex channel_mutex_;
	std::unordered_map<SOCKET, std::shared_ptr<Channel>> channels_;

};


#endif