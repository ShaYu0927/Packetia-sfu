#include "ShardedWorkerPool.h"
#include "RuntimeMetrics.h"
#include <chrono>
#include <future>
#include <iostream>
#include <stdexcept>
#include <thread>

#define CHECK(expr) do { if (!(expr)) throw std::runtime_error(#expr); } while (false)


int main()
try
{
    monitoring::MetricsContext context;
    auto registry = context.Registry();
    ShardedWorkerPool pool;
    std::promise<void> entered, release;
    auto gate = release.get_future().share();
    // Release the worker on every failure so pool destruction cannot deadlock.
    struct Release { std::promise<void>& promise; ~Release() { try { promise.set_value(); } catch (...) {} } } guard{release};
    CHECK(pool.start(1, nullptr, 2, ShardedWorkerPool::DropPolicy::DropTail, 1024, "metrics_worker") == 0);
    WorkJob slow;
    slow.type = WorkType::Function;
    slow.function = [&] { entered.set_value(); gate.wait(); };
    CHECK(pool.post(std::move(slow)) == 0);
    CHECK(entered.get_future().wait_for(std::chrono::seconds(3)) == std::future_status::ready);
    WorkJob queued;
    queued.type = WorkType::Function;
    queued.function = [] { throw std::runtime_error("expected handler error"); };
    CHECK(pool.post(std::move(queued)) == 0);
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
    auto view = registry->Collect().workers.at(0);
    CHECK(view.active_ns != 0 && view.queue_depth == 1);
    CHECK(monitoring::NowNs() - view.oldest_ns >= 10000000);
    CHECK(monitoring::NowNs() - view.active_ns >= 10000000);
    release.set_value();
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(3);
    do
    {
        view = registry->Collect().workers.at(0);
        if (view.execution.Count() == 2) break;
        std::this_thread::yield();
    } while (std::chrono::steady_clock::now() < deadline);
    CHECK(view.execution.Count() == 2 && view.queue_depth == 0);
    CHECK(view.queue_wait.MaxUs() >= 10000);
    CHECK(view.task_types[static_cast<size_t>(WorkType::Function)].execution.Count() == 2);
    pool.shutdown(true);
    CHECK(registry->Collect().workers.empty());
    std::cout << "Worker metrics capture queue stalls and throwing jobs with media tracing disabled\n";
    return 0;
}
catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
