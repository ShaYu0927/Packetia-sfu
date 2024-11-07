#include "TaskScheduler.h"

TaskScheduler::TaskScheduler(int id)
{
}

TaskScheduler::~TaskScheduler()
{
}

void TaskScheduler::start()
{
}

void TaskScheduler::stop()
{
}

TimeId TaskScheduler::AddTimer(TimeEvent timerEvent, uint32_t msec)
{
    return TimeId();
}

void TaskScheduler::RemoveTimer(TimeId timerId)
{
}

bool TaskScheduler::AddTriggerEvent(TriggerEvent callback)
{
    return false;
}
