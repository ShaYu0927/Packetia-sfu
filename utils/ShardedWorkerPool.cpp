#include "ShardedWorkerPool.h"
#include "logger.h"


std::mutex WorkerService::mtx_;
std::unordered_map<std::string, std::unique_ptr<ShardedWorkerPool>> WorkerService::pools_;

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

int ShardedWorkerPool::post(WorkJob &&job)
{
    if (!started_.load() || workers_.empty())
    {
        LOG_INFO("ShardedWorkerPool not started");
        return -1;
    }
    
    // if (job.key == 0)
    // {
    //     LOG_ERROR("post job with key=0, drop");
    //     safe_release_job(job);
    //     return -1;
    // }

    auto idx = shard_index(job.key);
    Worker& w = *workers_[idx];

    {
        std::unique_lock<std::mutex> lk(w.mtx);

        if (!w.running.load()) 
        {
            safe_release_job(job);
            return -1;
        }

        if (w.q.size() >= max_queue_len_)
        {
            // 队列满：按策略丢弃
            w.st.dropped++;
            if (drop_policy_ == DropPolicy::DropHead)
            {
                // 丢最老的：务必对被丢的旧 job 执行 deleter
                WorkJob old = std::move(w.q.front());
                w.q.pop_front();
                safe_release_job(old);
            }
            else
            {
                // 丢最新的：对当前 job 执行 deleter
                safe_release_job(job);
                return -1;
            }
        }

        printf("[Worker %zu] q=%zu enq=%lu drop=%lu max=%lu\n",
                idx,
                w.q.size(),
                w.st.dequeued,
                w.st.dropped,
                w.st.max_depth_seen);

         w.q.emplace_back(std::move(job));
         w.st.enqueued++;
        if (w.q.size() > w.st.max_depth_seen) w.st.max_depth_seen = w.q.size();
    }
    w.cv.notify_one();
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

void ShardedWorkerPool::worker_loop(Worker &w, std::size_t idx)
{
   
    while (w.running.load())
    {
        WorkJob job;
        {
            std::unique_lock<std::mutex> lk(w.mtx);
            w.cv.wait(lk, [&]{
                return !w.running.load() || !w.q.empty();
            });

            if (!w.running.load() && w.q.empty())
                break;

            job = std::move(w.q.front());
            w.q.pop_front();
            w.st.dequeued++;
        }

        
        if (job.deleter)
        {
            auto deleter = job.deleter;
            auto payload = job.payload;
            handler_->handle(std::move(job));
            WorkJob tmp{};
            tmp.payload = payload;
            deleter(tmp);
        }
        else
        {
            LOG_ERROR("job without deleter, key=%lu payload=%p", job.key, job.payload);
        }
        
    }

      
}

int WorkerService::create_pool(const std::string &name, std::size_t worker_count, std::shared_ptr<IJobHandler> handler, std::size_t max_queue_len, ShardedWorkerPool::DropPolicy drop)
{
    
    if (name.empty() || worker_count == 0 || !handler) return -1;
    std::lock_guard<std::mutex> lg(mtx_);
    if (pools_.count(name)) return 0;

    auto pool = std::make_unique<ShardedWorkerPool>();
    int ret = pool->start(worker_count, handler, max_queue_len, drop);

    if (ret != 0) return ret;
    pools_.emplace(name, std::move(pool));
    return 0;
}

void WorkerService::destroy_pool(const std::string &name, bool drain)
{
    std::unique_ptr<ShardedWorkerPool> pool;
    {
        std::lock_guard<std::mutex> lg(mtx_);
        auto it = pools_.find(name);
        if (it == pools_.end()) return;
        pool = std::move(it->second);
        pools_.erase(it);
    }
}

int WorkerService::post(const std::string &name, WorkJob &&job)
{
    ShardedWorkerPool* pool = nullptr;
    {
        std::lock_guard<std::mutex> lg(mtx_);
        auto it = pools_.find(name);
        if (it == pools_.end()) return -1;
        pool = it->second.get();
    }
    return pool->post(std::move(job));
}

bool WorkerService::exists(const std::string &name)
{
    std::lock_guard<std::mutex> lk(mtx_);
    return pools_.count(name) != 0;
}

ShardedWorkerPool *WorkerService::get_pool(const std::string &name)
{
    std::lock_guard<std::mutex> lg(mtx_);
    auto it = pools_.find(name);
    if (it != pools_.end())
    {
        return it->second.get();
    }
    return nullptr;
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
