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
#include <shared_mutex>
#include <memory>
#include <string>
#include <string_view>
#include <unordered_map>

#include "PacketPool.h"

enum class WorkType : uint32_t
{
    Invalid = 0,

    /* 媒体类 */
    Rtp,
    Rtcp,
    Stun,
    Dtls,

    /* 网络原始数据 */
    RawTcp,
    RawUdp,

    /* 控制/系统类 */
    Timer,
    Control,

    // Type-erased C++ task for globally managed business worker pools.
    Function
};

struct WorkJob
{
    // key selects a worker shard. target_id identifies the endpoint/session
    // that owns the job; keeping them separate allows per-stream affinity.
    uint64_t key = 0;
    uint64_t target_id = 0;
    WorkType type = WorkType::Invalid;

    union
    {
        Packet* pkt = nullptr;       // RTP / RTCP / STUN
        struct
        {
            uint8_t* data;
            uint32_t len;
        } raw;                      // TCP / UDP
    };

    uint64_t enqueue_ts = 0;

    // Optional type-erased owner for memory referenced by pkt/raw. Keeping
    // ownership on the job makes cross-thread views safe without coupling the
    // generic worker layer to a protocol-specific packet type.
    std::shared_ptr<void> owner;

    void (*handler)(WorkJob&, void* ctx) = nullptr;
    void (*deleter)(WorkJob&) = nullptr;

    std::function<void()> function;
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
        std::size_t   queue_depth = 0;
        std::size_t   worker_count = 0;
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
        std::lock_guard<std::mutex> lifecycle_lock(lifecycle_mtx_);
        ThreadStats sum;
        sum.worker_count = workers_.size();
        for (auto& up : workers_)
        {
            std::lock_guard<std::mutex> lock(up->mtx);
            sum.enqueued += up->st.enqueued;
            sum.dequeued += up->st.dequeued;
            sum.dropped  += up->st.dropped;
            sum.queue_depth += up->q.size();
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
    mutable std::mutex lifecycle_mtx_;

};



class WorkerService final 
{
public:
    enum class WorkerPoolId : std::size_t
    {
        Media = 0,
        Sip,
        Rtsp,
        Endpoint,
        Count,
    };

    struct PoolStatus
    {
        std::string name;
        ShardedWorkerPool::ThreadStats stats;
    };

    static int create_pool(const std::string& name,
                           std::size_t worker_count,
                           std::shared_ptr<IJobHandler> handler,
                           std::size_t max_queue_len = 2048,
                           ShardedWorkerPool::DropPolicy drop = ShardedWorkerPool::DropPolicy::DropHead);

    static int create_function_pool(
        std::string_view name,
        std::size_t worker_count,
        std::size_t max_queue_len = 2048,
        ShardedWorkerPool::DropPolicy drop = ShardedWorkerPool::DropPolicy::DropTail);

    static void destroy_pool(std::string_view name, bool drain);
    static void destroy_all(bool drain = true);

    static int post(std::string_view name, WorkJob&& job);

    static int post_fn(std::string_view name, std::function<void()> fn);
    static int post_fn(std::string_view name, std::uint64_t affinity_key,
                       std::function<void()> fn);

    static bool exists(std::string_view name);

    static std::shared_ptr<ShardedWorkerPool> get_pool_shared(std::string_view name);
    [[deprecated("use get_pool_shared() for lifetime safety")]]
    static ShardedWorkerPool* get_pool(std::string_view name);

    static std::vector<PoolStatus> status();
    static std::vector<std::string> pool_names();

    static void realse(Packet* p);

private:
    WorkerService() = delete;

    static std::string NormalizeName(std::string_view name);

    static std::shared_mutex mtx_;
    static std::unordered_map<std::string, std::shared_ptr<ShardedWorkerPool>> pools_;
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
