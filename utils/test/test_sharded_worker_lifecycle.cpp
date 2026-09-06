#include "ShardedWorkerPool.h"
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <map>
#include <mutex>
#include <stdexcept>
#include <thread>

#define CHECK(value) do { if (!(value)) throw std::runtime_error(#value); } while (false)

namespace {
class Handler final : public IJobHandler
{
public:
    void handle(WorkJob&) override {}

    void handle(WorkJob& job, size_t worker) override
    {
        std::lock_guard<std::mutex> lock(mutex);
        owners[job.key] = worker;
        ++handled;
        changed.notify_all();
    }

    void on_worker_tick(size_t) override
    {
        ++ticks;
        changed.notify_all();
    }

    void on_worker_stop(size_t) override
    {
        ++stops;
        changed.notify_all();
    }

    std::mutex mutex;
    std::condition_variable changed;
    std::map<uint64_t, size_t> owners;
    std::atomic<size_t> handled{0};
    std::atomic<size_t> ticks{0};
    std::atomic<size_t> stops{0};
};
}

int main()
{
    auto handler = std::make_shared<Handler>();
    ShardedWorkerPool pool;
    CHECK(pool.start(2, handler, 32, ShardedWorkerPool::DropPolicy::DropTail) == 0);
    for (int i = 0; i < 8; ++i) {
        WorkJob job{};
        job.key = 7;
        job.type = WorkType::Recording;
        CHECK(pool.post(std::move(job)) == 0);
    }
    {
        std::unique_lock<std::mutex> lock(handler->mutex);
        CHECK(handler->changed.wait_for(lock, std::chrono::seconds(3), [&] {
            return handler->handled == 8 && handler->ticks > 0;
        }));
        CHECK(handler->owners.size() == 1);
    }
    pool.shutdown(true);
    CHECK(handler->stops == 2);
    return 0;
}
