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
}

void SelectTaskScheduler::RemoveChannel(std::shared_ptr<Channel> channel)
{
}

bool SelectTaskScheduler::HandleEvent(int timeout)
{
    return false;
}
