#ifndef PACKETIA_KQUEUE_TASK_SCHEDULER_H_
#define PACKETIA_KQUEUE_TASK_SCHEDULER_H_

#include "TaskScheduler.h"

#include <cstdint>
#include <mutex>
#include <unordered_map>

class KqueueTaskScheduler : public TaskScheduler
{
public:
    explicit KqueueTaskScheduler(int id = 0);
    ~KqueueTaskScheduler() override;

    void UpdateChannel(std::shared_ptr<Channel> channel) override;
    void RemoveChannel(std::shared_ptr<Channel>& channel) override;
    bool HandleEvent(int timeout) override;

private:
    struct Registration
    {
        std::shared_ptr<Channel> channel;
        std::uintptr_t token = 0;
        int events = EVENT_NONE;
    };

    void SetRegistration(std::shared_ptr<Channel> channel, int events, bool removing);
    int SetFilters(int fd, int events, std::uintptr_t token);
    int ChangeFilter(int fd, short filter, bool enabled, std::uintptr_t token);

    int kqueuefd_ = -1;
    std::mutex channel_mutex_;
    std::unordered_map<int, Registration> channels_;
    std::uintptr_t next_token_ = 1;
};

#endif // PACKETIA_KQUEUE_TASK_SCHEDULER_H_
