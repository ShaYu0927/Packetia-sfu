#include "utils/MediaLatency.h"
#include "utils/ShardedWorkerPool.h"
#include "media/transport/MediaEndpointIngress.h"

#include <future>
#include <iostream>
#include <stdexcept>

#define CHECK(expr) do { if (!(expr)) throw std::runtime_error(#expr); } while (false)
using namespace media_latency;

class Handler final : public IJobHandler
{
public:
    std::promise<PacketTrace> packet;
    void handle(WorkJob& job) override { packet.set_value(job.media_trace); }
};

int main()
try
{
    auto handler = std::make_shared<Handler>();
    auto received = handler->packet.get_future();
    CHECK(WorkerService::create_pool(POOL_MEDIA, 1, handler, 1,
        ShardedWorkerPool::DropPolicy::DropTail) == 0);
    // Hold the worker deterministically while one RTP queues and one is rejected.
    std::promise<void> entered, release;
    auto released = release.get_future().share();
    auto entered_future = entered.get_future();
    CHECK(WorkerService::post_fn(POOL_MEDIA, [&] {
        entered.set_value();
        released.wait();
    }) == 0);
    entered_future.get();
    const uint8_t rtp[] = {0x80, 96, 0, 1, 0, 0, 0, 1, 0, 0, 0, 42, 1};
    media::transport::MediaEndpointIngress ingress(7);
    const auto before = NowNs();
    CHECK(ingress.OnMediaPacket(ReceivedMediaPacket(MediaPacketType::Rtp, 7, 999999999,
        rtp, sizeof(rtp))) == MediaPacketIngressResult::Accepted);
    CHECK(ingress.OnMediaPacket(ReceivedMediaPacket(MediaPacketType::Rtp, 7, 999999999,
        rtp, sizeof(rtp))) == MediaPacketIngressResult::Dropped);
    const auto after_queue = NowNs();
    release.set_value();
    const auto trace = received.get();
    WorkerService::destroy_all(true);
    CHECK(trace.ingress_ns >= before);
    CHECK(trace.enqueue_ns >= trace.ingress_ns && trace.enqueue_ns <= after_queue);
    CHECK(trace.worker_ns >= after_queue);
    const auto stats = TakeSnapshot();
    CHECK(stats.counters[static_cast<std::size_t>(Counter::IngressSampled)] == 2);
    CHECK(stats.counters[static_cast<std::size_t>(Counter::QueueDropped)] == 1);
    std::cout << "RTP ingress/queue/worker timestamps and overload accounting passed\n";
    return 0;
}
catch (const std::exception& error)
{
    std::cerr << error.what() << '\n';
    return 1;
}
