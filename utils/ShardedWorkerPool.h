#ifndef _SHARDEDWORKERPOOL_H_
#define _SHARDEDWORKERPOOL_H_

#include <cstdint>
#include <cstddef>
#include <vector>
#include <deque>
#include <thread>
#include <mutex>
#include <condition_variable>
#include <atomic>
#include <functional>

#include "PacketPool.h"

typedef struct WorkJob 
{
    uint64_t key;                // 用于分片/串行（例如 track_id / session_id / stream_id）
    uint32_t type;               // 任务类型（可选）
    void*    payload;            // 业务指针（或 std::variant）
    size_t   payload_len;        // 业务长度（可选）
    uint64_t enqueue_ts;         // 观测
}WorkJob;


struct IJobHandler
{
    virtual ~IJobHandler() = default;
    virtual void handle(WorkJob&& job) = 0;
};


class ShardedWorkerPool
{
public:
    enum class DropPolicy { DropHead, DropTail };

    typedef struct ThreadStats
    {
        std::uint64_t enqueued = 0;
        std::uint64_t dequeued = 0;
        std::uint64_t dropped  = 0;
        std::size_t   max_depth_seen = 0;
    }ThreadStats;

    ShardedWorkerPool() = default;

    ~ShardedWorkerPool() { shutdown(true); }

    void shutdown(bool drain);

    int post(WorkJob&& job);

    int start(std::size_t worker_count,
               std::shared_ptr<IJobHandler> handler,
               std::size_t max_queue_len = 4096,
               DropPolicy drop = DropPolicy::DropHead);

    ThreadStats Status() const
    {
        ThreadStats sum;
        for (auto& up : workers_)
        {
            // 读统计不强求强一致，简单读即可；如要强一致可加锁
            sum.enqueued += up->st.enqueued;
            sum.dequeued += up->st.dequeued;
            sum.dropped  += up->st.dropped;
            if (up->st.max_depth_seen > sum.max_depth_seen)
                sum.max_depth_seen = up->st.max_depth_seen;
        }
        return sum;
    }

private:

    struct Worker
    {
        std::mutex mtx;
        std::condition_variable cv;
        std::deque<WorkJob> q;
        std::thread th;

        std::atomic<bool> running{false};
        std::atomic<bool> draining{false};

        ThreadStats st;
    };

    std::size_t shard_index(std::uint64_t key) const;

    void worker_loop(Worker& w, std::size_t idx);

    std::vector<std::unique_ptr<Worker>> workers_;
    std::shared_ptr<IJobHandler> handler_;

    std::size_t max_queue_len_ = 4096;
    DropPolicy drop_policy_ = DropPolicy::DropHead;
    std::atomic<bool> started_{false};

};


class WorkerService final 
{
public:
    static int create_pool(const std::string& name,
                           std::size_t worker_count,
                           std::shared_ptr<IJobHandler> handler,
                           std::size_t max_queue_len = 2048,
                           ShardedWorkerPool::DropPolicy drop = ShardedWorkerPool::DropPolicy::DropHead);

    static void destroy_pool(const std::string& name, bool drain);

    static int post(const std::string& name, WorkJob&& job);

    static bool exists(const std::string& name);

private:
    WorkerService() = delete;

    static std::mutex mtx_;
    static std::unordered_map<std::string, std::unique_ptr<ShardedWorkerPool>> pools_;
};
#endif