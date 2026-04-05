#include "ShardedWorkerPool.h"
#include "logger.h"

namespace
{
using PoolId = WorkerService::WorkerPoolId;

inline std::size_t ToIndex(PoolId id)
{
    return static_cast<std::size_t>(id);
}

bool ParsePoolName(const std::string& name, PoolId& id)
{
    if (name == "Media" || name == "media")
    {
        id = PoolId::Media;
        return true;
    }
    if (name == "Sip" || name == "sip")
    {
        id = PoolId::Sip;
        return true;
    }
    if (name == "Rtsp" || name == "rtsp")
    {
        id = PoolId::Rtsp;
        return true;
    }
    if (name == "endpoint_pool" || name == "EndpointPool")
    {
        id = PoolId::Endpoint;
        return true;
    }
    return false;
}

ShardedWorkerPool* GetPoolUnlocked(
    std::array<std::unique_ptr<ShardedWorkerPool>,
               static_cast<std::size_t>(PoolId::Count)>& pools,
    const std::string& name)
{
    PoolId id;
    if (!ParsePoolName(name, id))
    {
        return nullptr;
    }

    return pools[ToIndex(id)].get();
}

} // namespace

std::mutex WorkerService::mtx_;

std::array<std::unique_ptr<ShardedWorkerPool>,
           static_cast<std::size_t>(WorkerService::WorkerPoolId::Count)>
    WorkerService::pools_ = {};


static std::atomic<uint64_t> g_post_calls{0};

static inline std::uint64_t simple_hash_u64(std::uint64_t x)
{
    x ^= x >> 33;
    x *= 0xff51afd7ed558ccdULL;
    x ^= x >> 33;
    x *= 0xc4ceb9fe1a85ec53ULL;
    x ^= x >> 33;
    return x;
}

void ShardedWorkerPool::shutdown(bool drain)
{
    if (!started_.exchange(false)) return;

    for (auto& up : workers_)
    {
        Worker& w = *up;
        {
            std::unique_lock<std::mutex> lk(w.mtx);
            w.draining.store(drain);
            w.running.store(false);
        }
        w.cv.notify_one();
    }

    for (auto& up : workers_)
    {
        if (up->th.joinable()) up->th.join();
    }

    /* to avoid the need to release residual payloads */
    if (!drain)
    {
        for (auto& up : workers_)
        {
            std::unique_lock<std::mutex> lk(up->mtx);
            up->q.clear();
        }
    }

    workers_.clear();
    handler_.reset();
}

static inline void safe_release_job(WorkJob& job)
{
    if (job.deleter) job.deleter(job);
}

int ShardedWorkerPool::post(WorkJob&& job)
{
    if (!started_.load() || workers_.empty())
    {
        LOG_ERROR("[worker post] pool not started, key={}, type={}",
                  job.key, static_cast<int>(job.type));
        safe_release_job(job);
        return -1;
    }

    auto idx = shard_index(job.key);
    Worker& shard_worker = *workers_[idx];
    {
        std::unique_lock<std::mutex> lk(shard_worker.mtx);

        const size_t before_size = shard_worker.q.size();
        if (!shard_worker.running.load())
        {
            LOG_ERROR("[worker post] shard not running, key={}, type={}, shard={}, queue_size={}",
                      job.key,
                      static_cast<int>(job.type),
                      idx,
                      before_size);
            safe_release_job(job);
            return -1;
        }

        if (before_size >= max_queue_len_)
        {
            shard_worker.st.dropped++;

            LOG_ERROR("[worker post] queue full, key={}, type={}, shard={}, queue_size={}, max_queue_len={}, drop_policy={}",
                      job.key,
                      static_cast<int>(job.type),
                      idx,
                      before_size,
                      max_queue_len_,
                      static_cast<int>(drop_policy_));

            if (drop_policy_ == DropPolicy::DropHead)
            {
                WorkJob old = std::move(shard_worker.q.front());

                LOG_ERROR("[worker post] drop head, shard={}, old_key={}, old_type={}",
                          idx,
                          old.key,
                          static_cast<int>(old.type));

                shard_worker.q.pop_front();
                safe_release_job(old);
            }
            else
            {
                LOG_ERROR("[worker post] drop tail/current job, shard={}, key={}, type={}",
                          idx,
                          job.key,
                          static_cast<int>(job.type));
                safe_release_job(job);
                return -1;
            }
        }
        shard_worker.q.emplace_back(std::move(job));
        shard_worker.st.enqueued++;

        const size_t after_size = shard_worker.q.size();
        if (after_size > shard_worker.st.max_depth_seen)
        {
            shard_worker.st.max_depth_seen = after_size;
        }
    }
    shard_worker.cv.notify_one();
    return 0;
}

int ShardedWorkerPool::start(std::size_t worker_count, std::shared_ptr<IJobHandler> handler, std::size_t max_queue_len, DropPolicy drop)
{
    if(!handler || worker_count == 0) return -1;

    if (started_.exchange(true)) return -1;

    handler_ = std::move(handler);
    max_queue_len_ = (max_queue_len == 0) ? 1 : max_queue_len;
    drop_policy_ = drop;

    workers_.reserve(worker_count);

    for(std::size_t t = 0;t < worker_count; t++)
    {
        auto w = std::make_unique<Worker>();
        w->running.store(true);
        w->draining.store(false);
        w->th = std::thread([this, wp = w.get(), t]() { worker_loop(*wp, t); });
        workers_.push_back(std::move(w));
    }
    return 0;
}

std::size_t ShardedWorkerPool::shard_index(std::uint64_t key) const
{
    auto h = simple_hash_u64(key);
    return static_cast<std::size_t>(h % workers_.size());
}

void ShardedWorkerPool::worker_loop(Worker &worker, std::size_t idx)
{
    for (;;)
    {
        WorkJob job;
        {
            std::unique_lock<std::mutex> lk(worker.mtx);

            worker.cv.wait(lk, [&] {
                return !worker.running.load() || !worker.q.empty();
            });

            if (!worker.running.load() && worker.q.empty())
            {
                LOG_INFO("worker exit by stop, idx=", idx);
                break;
            }

            job = std::move(worker.q.front());
            worker.q.pop_front();
            worker.st.dequeued++;
        }

        if (!handler_)
        {
            LOG_ERROR("worker handler is null, idx=", idx);
            continue;
        }
        handler_->handle(job);
    }
}

int WorkerService::create_pool(const std::string &name, std::size_t worker_count, std::shared_ptr<IJobHandler> handler, std::size_t max_queue_len, ShardedWorkerPool::DropPolicy drop)
{
    LOG_INFO("[WorkerService] create_pool called, name=", name,
             " worker_count=", worker_count,
             " max_queue_len=", max_queue_len,
             " drop_policy=", static_cast<int>(drop));
    PoolId id;
    if (!ParsePoolName(name, id))
    {
        LOG_ERROR("[WorkerService] unknown pool name: ", name);
        return -1;
    }

    if (!handler)
    {
        LOG_ERROR("[WorkerService] handler is null, pool=" + name);
        return -2;
    }

    auto pool = std::make_unique<ShardedWorkerPool>();
    const int ret = pool->start(worker_count, std::move(handler), max_queue_len, drop);
    if (ret != 0)
    {
        LOG_ERROR("[WorkerService] start pool failed, pool=" + name + " ret=" + std::to_string(ret));
        return ret;
    }

    {
        std::lock_guard<std::mutex> lg(mtx_);
        auto& slot = pools_[ToIndex(id)];
        if (slot)
        {
            slot.reset();
        }
        slot = std::move(pool);
    }

    std::cout << "[WorkerService] create pool ok, pool=" << name
              << " worker_count=" << worker_count
              << " max_queue_len=" << max_queue_len << std::endl;
    return 0;
}

void WorkerService::destroy_pool(const std::string &name, bool drain)
{
     std::unique_ptr<ShardedWorkerPool> pool;

    {
        std::lock_guard<std::mutex> lg(mtx_);

        PoolId id;
        if (!ParsePoolName(name, id))
        {
            std::cerr << "[WorkerService] unknown pool name: " << name << std::endl;
            return;
        }

        auto& slot = pools_[ToIndex(id)];
        if (!slot)
        {
            return;
        }

        pool = std::move(slot);
    }

    std::cout << "[WorkerService] destroy pool ok, pool=" << name
              << " drain=" << (drain ? "true" : "false") << std::endl;
}

int WorkerService::post(const std::string &name, WorkJob &&job)
{
    std::lock_guard<std::mutex> lg(mtx_);

    ShardedWorkerPool* pool = GetPoolUnlocked(pools_, name);
    if (!pool)
    {
        std::cerr << "[WorkerService] post failed, pool not found: " << name << std::endl;
        return -1;
    }

    return pool->post(std::move(job));
}


bool WorkerService::exists(const std::string &name)
{
    std::lock_guard<std::mutex> lg(mtx_);
    return GetPoolUnlocked(pools_, name) != nullptr;
}

ShardedWorkerPool *WorkerService::get_pool(const std::string &name)
{
    std::lock_guard<std::mutex> lg(mtx_);
    return GetPoolUnlocked(pools_, name);
}

void WorkerService::realse(Packet *p)
{
    if (!p) return;

    PacketPool* pool = p->owner;
    if (!pool)
    {
        LOG_ERROR("Packet without pool, leak or corruption");
        return;
    }

    pool->release(p);
}
