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

enum class WorkType : uint32_t
{
    Invalid = 0,

    /* 媒体类 */
    Rtp,
    Rtcp,
    Stun,

    /* 网络原始数据 */
    RawTcp,
    RawUdp,

    /* 控制/系统类 */
    Timer,
    Control
};

struct WorkJob
{
    uint64_t key;
    WorkType type = WorkType::Invalid;

    union
    {
        Packet* pkt;                 // RTP / RTCP / STUN
        struct
        {
            uint8_t* data;
            uint32_t len;
        } raw;                      // TCP / UDP
    };

    uint64_t enqueue_ts;

    void (*handler)(WorkJob&, void* ctx);
    void (*deleter)(WorkJob&) = nullptr;
};

struct SessionEntry
{
    uint32_t type;   // UDP / TCP / HTTP / 
    void*    ptr;
};

struct WorkerContext
{
    std::unordered_map<uint64_t, SessionEntry> sessions;
};

enum : uint32_t
{
    WORKJOB_TYPE_UNKNOWN = 0,

    WORKJOB_TYPE_TCP_PACKET,
    WORKJOB_TYPE_UDP_PACKET,

    WORKJOB_TYPE_RTSP,
    WORKJOB_TYPE_RTP,
    WORKJOB_TYPE_RTCP,
    WORKJOB_TYPE_STUN,
    WORKJOB_TYPE_DTLS,

    WORKJOB_TYPE_FN,
};

struct IJobHandler
{
    virtual ~IJobHandler() = default;
    virtual void handle(WorkJob& job) = 0;
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
    enum class WorkerPoolId : std::size_t
    {
        Media = 0,
        Sip,
        Rtsp,
        Count
    };

    static int create_pool(const std::string& name,
                           std::size_t worker_count,
                           std::shared_ptr<IJobHandler> handler,
                           std::size_t max_queue_len = 2048,
                           ShardedWorkerPool::DropPolicy drop = ShardedWorkerPool::DropPolicy::DropHead);

    static void destroy_pool(const std::string& name, bool drain);

    static int post(const std::string& name, WorkJob&& job);

    static int post_fn(const std::string& name, std::function<void()> fn);

    static bool exists(const std::string& name);

    static ShardedWorkerPool* get_pool(const std::string& name);

    static void realse(Packet* p);

private:
    WorkerService() = delete;

    static std::mutex mtx_;
    static std::array<std::unique_ptr<ShardedWorkerPool>,
           static_cast<std::size_t>(WorkerPoolId::Count)> pools_;
};

struct WorkerModuleConfig
{
    WorkerService::WorkerPoolId id;
    std::size_t worker_count;
    std::size_t max_queue_len;
    ShardedWorkerPool::DropPolicy drop_policy;
    std::shared_ptr<IJobHandler> handler;
};

#endif