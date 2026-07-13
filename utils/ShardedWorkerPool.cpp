#include "ShardedWorkerPool.h"
#include "logger.h"

#include <algorithm>
#include <cctype>
#include <exception>
#include <iterator>
#include <utility>

namespace
{
constexpr std::string_view kEndpointPoolAlias = "endpointpool";
constexpr std::string_view kEndpointPoolName = "endpoint_pool";

} // namespace

std::shared_mutex WorkerService::mtx_;
std::unordered_map<std::string, std::shared_ptr<ShardedWorkerPool>>
    WorkerService::pools_;


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
    {
        std::lock_guard<std::mutex> lifecycle_lock(lifecycle_mtx_);

        if (!started_.exchange(false)) return;

        for (auto& up : workers_)
        {
            Worker& w = *up;
            {
                std::unique_lock<std::mutex> lk(w.mtx);
                w.draining.store(drain);
                w.running.store(false);
                if (!drain)
                {
                    while (!w.q.empty())
                    {
                        WorkJob job = std::move(w.q.front());
                        w.q.pop_front();
                        if (job.deleter) job.deleter(job);
                    }
                }
            }
            w.cv.notify_one();
        }
    }

    for (auto& up : workers_)
    {
        if (up->th.joinable()) up->th.join();
    }

    {
        std::lock_guard<std::mutex> lifecycle_lock(lifecycle_mtx_);
        workers_.clear();
        handler_.reset();
    }
}

static inline void safe_release_job(WorkJob& job)
{
    if (job.deleter) job.deleter(job);
}

int ShardedWorkerPool::post(WorkJob&& job)
{
    std::lock_guard<std::mutex> lifecycle_lock(lifecycle_mtx_);

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
    std::lock_guard<std::mutex> lifecycle_lock(lifecycle_mtx_);

    if (worker_count == 0) return -1;

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

        if (job.type == WorkType::Function)
        {
            try
            {
                if (job.function)
                {
                    job.function();
                }
            }
            catch (const std::exception& ex)
            {
                LOG_ERROR("worker function threw exception, idx=", idx,
                          " what=", ex.what());
            }
            catch (...)
            {
                LOG_ERROR("worker function threw unknown exception, idx=", idx);
            }
            safe_release_job(job);
            continue;
        }

        if (!handler_)
        {
            LOG_ERROR("worker handler is null, idx=", idx);
            safe_release_job(job);
            continue;
        }
        handler_->handle(job);
    }
}

std::string WorkerService::NormalizeName(std::string_view name)
{
    std::string normalized;
    normalized.reserve(name.size());
    std::transform(name.begin(), name.end(), std::back_inserter(normalized),
                   [](unsigned char ch) {
                       return static_cast<char>(std::tolower(ch));
                   });

    if (normalized == kEndpointPoolAlias)
    {
        normalized.assign(kEndpointPoolName.data(), kEndpointPoolName.size());
    }
    return normalized;
}

int WorkerService::create_pool(const std::string& name,
                               std::size_t worker_count,
                               std::shared_ptr<IJobHandler> handler,
                               std::size_t max_queue_len,
                               ShardedWorkerPool::DropPolicy drop)
{
    const auto normalized_name = NormalizeName(name);
    if (normalized_name.empty() || !handler)
    {
        LOG_ERROR("[WorkerService] invalid pool configuration, name=", name,
                  " handler=", static_cast<bool>(handler));
        return -1;
    }

    auto pool = std::make_shared<ShardedWorkerPool>();
    const int ret = pool->start(worker_count, std::move(handler), max_queue_len, drop);
    if (ret != 0)
    {
        LOG_ERROR("[WorkerService] start pool failed, pool=", normalized_name,
                  " ret=", ret);
        return ret;
    }

    std::shared_ptr<ShardedWorkerPool> old_pool;
    {
        std::unique_lock<std::shared_mutex> lock(mtx_);
        auto [it, inserted] = pools_.try_emplace(normalized_name, pool);
        if (!inserted)
        {
            old_pool = std::exchange(it->second, std::move(pool));
        }
    }

    if (old_pool)
    {
        old_pool->shutdown(true);
    }

    LOG_INFO("[WorkerService] create pool ok, pool=", normalized_name,
             " worker_count=", worker_count,
             " max_queue_len=", max_queue_len);
    return 0;
}

int WorkerService::create_function_pool(std::string_view name,
                                        std::size_t worker_count,
                                        std::size_t max_queue_len,
                                        ShardedWorkerPool::DropPolicy drop)
{
    const auto normalized_name = NormalizeName(name);
    if (normalized_name.empty())
    {
        return -1;
    }

    auto pool = std::make_shared<ShardedWorkerPool>();
    const int ret = pool->start(worker_count, nullptr, max_queue_len, drop);
    if (ret != 0)
    {
        return ret;
    }

    std::shared_ptr<ShardedWorkerPool> old_pool;
    {
        std::unique_lock<std::shared_mutex> lock(mtx_);
        auto [it, inserted] = pools_.try_emplace(normalized_name, pool);
        if (!inserted)
        {
            old_pool = std::exchange(it->second, std::move(pool));
        }
    }

    if (old_pool)
    {
        old_pool->shutdown(true);
    }

    LOG_INFO("[WorkerService] create function pool ok, pool=", normalized_name,
             " worker_count=", worker_count,
             " max_queue_len=", max_queue_len);
    return 0;
}

void WorkerService::destroy_pool(std::string_view name, bool drain)
{
    const auto normalized_name = NormalizeName(name);
    std::shared_ptr<ShardedWorkerPool> pool;
    {
        std::unique_lock<std::shared_mutex> lock(mtx_);
        auto node = pools_.extract(normalized_name);
        if (node.empty())
        {
            return;
        }
        pool = std::move(node.mapped());
    }

    pool->shutdown(drain);
    LOG_INFO("[WorkerService] destroy pool ok, pool=", normalized_name,
             " drain=", drain);
}

void WorkerService::destroy_all(bool drain)
{
    std::unordered_map<std::string, std::shared_ptr<ShardedWorkerPool>> pools;
    {
        std::unique_lock<std::shared_mutex> lock(mtx_);
        pools.swap(pools_);
    }

    for (auto& [name, pool] : pools)
    {
        if (pool)
        {
            pool->shutdown(drain);
            LOG_INFO("[WorkerService] destroy pool ok, pool=", name,
                     " drain=", drain);
        }
    }
}

int WorkerService::post(std::string_view name, WorkJob&& job)
{
    auto pool = get_pool_shared(name);
    if (!pool)
    {
        LOG_ERROR("[WorkerService] post failed, pool not found: ", std::string(name));
        safe_release_job(job);
        return -1;
    }
    return pool->post(std::move(job));
}

int WorkerService::post_fn(std::string_view name, std::function<void()> fn)
{
    return post_fn(name, 0, std::move(fn));
}

int WorkerService::post_fn(std::string_view name,
                           std::uint64_t affinity_key,
                           std::function<void()> fn)
{
    if (!fn)
    {
        return -1;
    }

    WorkJob job{};
    job.key = affinity_key;
    job.type = WorkType::Function;
    job.function = std::move(fn);
    return post(name, std::move(job));
}

bool WorkerService::exists(std::string_view name)
{
    std::shared_lock<std::shared_mutex> lock(mtx_);
    return pools_.find(NormalizeName(name)) != pools_.end();
}

std::shared_ptr<ShardedWorkerPool> WorkerService::get_pool_shared(std::string_view name)
{
    std::shared_lock<std::shared_mutex> lock(mtx_);
    auto it = pools_.find(NormalizeName(name));
    return it == pools_.end() ? nullptr : it->second;
}

ShardedWorkerPool* WorkerService::get_pool(std::string_view name)
{
    // Preserve the legacy raw-pointer API without returning an immediately
    // dangling object if another thread removes the global registration.
    thread_local std::shared_ptr<ShardedWorkerPool> keep_alive;
    keep_alive = get_pool_shared(name);
    return keep_alive.get();
}

std::vector<WorkerService::PoolStatus> WorkerService::status()
{
    std::vector<std::pair<std::string, std::shared_ptr<ShardedWorkerPool>>> snapshot;
    {
        std::shared_lock<std::shared_mutex> lock(mtx_);
        snapshot.reserve(pools_.size());
        for (const auto& [name, pool] : pools_)
        {
            snapshot.emplace_back(name, pool);
        }
    }

    std::vector<PoolStatus> result;
    result.reserve(snapshot.size());
    for (const auto& [name, pool] : snapshot)
    {
        if (pool)
        {
            result.push_back(PoolStatus{name, pool->Status()});
        }
    }
    std::sort(result.begin(), result.end(),
              [](const PoolStatus& lhs, const PoolStatus& rhs) {
                  return lhs.name < rhs.name;
              });
    return result;
}

std::vector<std::string> WorkerService::pool_names()
{
    auto statuses = status();
    std::vector<std::string> names;
    names.reserve(statuses.size());
    std::transform(statuses.begin(), statuses.end(), std::back_inserter(names),
                   [](const PoolStatus& item) { return item.name; });
    return names;
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
