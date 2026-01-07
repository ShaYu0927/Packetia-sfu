#include "ShardedWorkerPool.h"


std::mutex WorkerService::mtx_;
std::unordered_map<std::string, std::unique_ptr<ShardedWorkerPool>> WorkerService::pools_;

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

int ShardedWorkerPool::post(WorkJob &&job)
{
    if (!started_.load() || workers_.empty()) return -1;

    auto idx = shard_index(job.key);
    Worker& w = *workers_[idx];

    {
        std::unique_lock<std::mutex> lk(w.mtx);

        if (!w.running.load()) return -1;

        if (w.q.size() >= max_queue_len_)
        {
            // 队列满：按策略丢弃
            w.st.dropped++;
            if (drop_policy_ == DropPolicy::DropHead)
            {
                // 丢最老的，保证实时性
                w.q.pop_front();
            }
            else
            {
                // 丢最新的
                return false;
            }
        }

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
        w->running.store(false);
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
