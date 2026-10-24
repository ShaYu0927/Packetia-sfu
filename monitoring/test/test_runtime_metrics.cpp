#include "RuntimeMetrics.h"
#include <iostream>
#include <stdexcept>
#include <thread>
#include <vector>
#define CHECK(expr) do { if (!(expr)) throw std::runtime_error(#expr); } while (false)
using namespace monitoring;
void Run()
{
    MetricsContext context;
    auto registry = context.Registry();
    Histogram histogram;
    for (uint64_t i = 1; i <= 100; ++i) histogram.Add(i * 1000000);
    CHECK(histogram.Count() == 100);
    CHECK(histogram.PercentileUs(95) >= 95000 && histogram.PercentileUs(95) <= 105000);
    CHECK(histogram.PercentileUs(99) >= 99000 && histogram.PercentileUs(99) <= 110000);
    auto worker = RegisterWorker("test", 2, {"invalid", "rtp"});
    auto now = NowNs();
    worker->Queue(3, 42, now - 4000000, 7);
    worker->Begin(now - 3000000, 1, now - 1000000);
    auto view = registry->Collect().workers.at(0);
    CHECK(view.active_ns && view.queue_bytes == 42 && view.queue_depth == 3 && view.dropped == 7);
    CHECK(view.queue_wait.Count() == 1 && view.execution.Count() == 0);
    worker->End(1, now);
    CHECK(view.execution.Count() == 0); // Previously collected values are immutable copies.
    view = registry->Collect().workers.at(0);
    CHECK(view.task_types[1].execution.MaxUs() == 1000.0 && view.active_ns == 0);
    worker.reset();
    CHECK(registry->Collect().workers.empty());
    auto queue = RegisterSendQueue("tcp");
    queue->Set(123, 2, NowNs() - 2000000);
    CHECK(registry->Collect().queues.at(0).bytes == 123);
    queue->Set(0, 0, 0);
    CHECK(registry->Collect().queues.empty());
    queue.reset();
    std::vector<std::thread> threads;
    for (int t = 0; t < 4; ++t)
        threads.emplace_back([] { for (int i = 0; i < 1000; ++i) RecordSend(Transport::Udp, SendEvent::Sent, 1200, 1); });
    for (int i = 0; i < 5; ++i) registry->Collect();
    for (auto& thread : threads) thread.join();
    RecordSend(Transport::Udp, SendEvent::Blocked);
    RecordSend(Transport::Udp, SendEvent::Failed);
    auto send = registry->Collect().send[1];
    CHECK(send.bytes == 4800000 && send.packets == 4000);
    CHECK(send.blocked == 1 && send.failures == 2 && send.bitrate_bps > 0);
    CHECK(registry->Collect().send[1].packets == 4000);
    MetricRegistry independent;
    CHECK(independent.Collect().workers.empty() && independent.Collect().send[1].packets == 0);
}
int main()
try
{
    Run();
    CHECK(!RegisterWorker("unbound", 0) && !RegisterSendQueue("tcp"));
    RecordSend(Transport::Udp, SendEvent::Failed); // No-op after application teardown.
    MetricsContext fresh;
    CHECK(fresh.Registry()->Collect().send[1].packets == 0);
    std::cout << "Core metrics: typed snapshots, concurrency, application isolation and cleanup passed\n";
    return 0;
}
catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
