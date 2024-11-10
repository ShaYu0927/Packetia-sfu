#ifndef _SELEC_TASK_SCHEDULER_H_
#define _SELEC_TASK_SCHEDULER_H_

#include <sys/select.h>
#include <sys/time.h>
#include <sys/types.h>
#include <unistd.h>
#include <forward_list>

#include "network/Socket.h"
#include "TaskScheduler.h"
/**
 * 
 * 管理和处理网络或 I/O 事件的
 */

class SelectTaskScheduler : public TaskScheduler
{
public:
    SelectTaskScheduler(int id = 0);
    virtual ~SelectTaskScheduler();

    void UpdateChannel(std::shared_ptr<Channel> channel);
    void RemoveChannel(std::shared_ptr<Channel> channel);
    bool HandleEvent(int timeout);

private:
    //监控描述符
    fd_set fd_read_backup_;
    fd_set fd_write_backup_;
    fd_set fd_exp_backup_;
    int maxfd_ = 0;

    bool is_fd_read_reset_ = false;
	bool is_fd_write_reset_ = false;
	bool is_fd_exp_reset_ = false;

	std::mutex mutex_;
	std::unordered_map<SOCKET, std::shared_ptr<Channel>> channels_;


};


#endif