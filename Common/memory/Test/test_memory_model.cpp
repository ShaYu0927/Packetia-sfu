#include "Common/memory/SharedBuffer.h"
#include "Common/Depacketizer/AudioDepacketizer.h"
#include "Common/Depacketizer/H264Depacketizer.h"
#include "Rtsp/Rtp.h"
#include "Rtsp/RtpReceiver.h"
#include "media/core/EncodedFrameRouter.h"
#include "media/transport/MediaEndpointIngress.h"
#include "utils/EndpointBase.h"
#include "utils/MediaStreamAffinity.h"
#include "utils/ShardedWorkerPool.h"

#include <atomic>
#include <chrono>
#include <future>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <thread>
#include <type_traits>

#define CHECK(value) do { if (!(value)) throw std::runtime_error(#value); } while (false)
using common::BufferView;
using common::SharedBuffer;
using namespace std::chrono_literals;

static_assert(std::is_same_v<decltype(std::declval<SharedBuffer&>().Data()), const uint8_t*>);
static_assert(!std::is_copy_constructible_v<WorkJob>);

namespace
{
template<class Error, class Fn> void Throws(Fn fn)
{
    try { fn(); } catch (const Error&) { return; }
    throw std::runtime_error("expected exception");
}

SharedBuffer Tracked(std::atomic<int>& released, size_t size = 32)
{
    auto deleter = [&released](uint8_t* data) noexcept { delete[] data; ++released; };
    std::unique_ptr<uint8_t[], decltype(deleter)> bytes(new uint8_t[size]{}, deleter);
    return SharedBuffer::Take(std::move(bytes), size);
}

void BufferOwnershipAndBounds()
{
    CHECK(SharedBuffer{}.Data() == nullptr);
    CHECK(SharedBuffer::Copy(nullptr, 0).Empty());
    Throws<std::invalid_argument>([] { SharedBuffer::Copy(nullptr, 1); });
    Throws<std::invalid_argument>([] { SharedBuffer::Take(std::unique_ptr<uint8_t[]>{}, 1); });

    std::vector<uint8_t> source{1, 2, 3, 4};
    auto copy = SharedBuffer::Copy(source.data(), source.size());
    source[0] = 9;
    CHECK(copy.Data()[0] == 1);
    const auto* allocation = source.data();
    auto owned = SharedBuffer::Take(std::move(source));
    CHECK(owned.Data() == allocation);
    auto slice = owned.Slice(1, 2);
    auto nested = slice.Slice(1, 1);
    CHECK(slice.Data() == allocation + 1);
    owned = {};
    slice = {};
    CHECK(nested.Data()[0] == 3);
    Throws<std::out_of_range>([&] { nested.Slice(2, 0); });
    Throws<std::out_of_range>([&] { nested.Slice(1, 1); });
    Throws<std::out_of_range>([&] { nested.Slice(1, std::numeric_limits<size_t>::max()); });
    CHECK(nested.Slice(1, 0).Empty());
    auto moved = std::move(nested);
    CHECK(nested.Empty() && nested.Data() == nullptr);
    CHECK(moved.ToVector() == std::vector<uint8_t>{3});
    nested = std::move(moved);
    CHECK(moved.Empty() && moved.Data() == nullptr);
    auto writable = nested.ToVector();
    writable[0] = 99;
    CHECK(nested.Data()[0] == 3);
}

void LastOwnerCanOutliveProducerAndPoolHandle()
{
    struct State { std::atomic<int> releases{0}; };
    auto state = std::make_shared<State>();
    std::weak_ptr<State> weak = state;
    std::promise<SharedBuffer> produced;
    auto result = produced.get_future();
    std::thread producer([state, &produced] {
        auto deleter = [state](uint8_t* data) noexcept { delete[] data; ++state->releases; };
        std::unique_ptr<uint8_t[], decltype(deleter)> bytes(new uint8_t[8]{1, 2}, deleter);
        auto buffer = SharedBuffer::Take(std::move(bytes), 8);
        produced.set_value(buffer.Slice(1, 1));
    });
    producer.join();
    auto buffer = result.get();
    CHECK(buffer.Data()[0] == 2);
    CHECK(state->releases == 0);
    state.reset();
    CHECK(!weak.expired());
    std::thread consumer([buffer = std::move(buffer)]() mutable { buffer = {}; });
    consumer.join();
    CHECK(weak.expired());
}

class CallbackHandler final : public IJobHandler
{
public:
    std::function<void(WorkJob&)> callback;
    void handle(WorkJob& job) override { if (callback) callback(job); }
};

void RejectedJobsReleaseImmediately()
{
    std::atomic<int> released{0};
    ShardedWorkerPool pool;
    WorkJob first;
    first.payload = Tracked(released);
    CHECK(pool.post(std::move(first)) != 0);
    CHECK(first.payload.Empty() && released == 1);
    WorkJob second;
    second.payload = Tracked(released);
    CHECK(WorkerService::post("memory-test-missing", std::move(second)) != 0);
    CHECK(second.payload.Empty() && released == 2);
}

// A barrier keeps the worker busy so queue overflow is deterministic.
void QueueDropsReleaseExactlyOnce(ShardedWorkerPool::DropPolicy policy, bool drain)
{
    std::atomic<int> released{0}, handled{0};
    std::promise<void> entered, resume;
    auto started = entered.get_future();
    auto gate = resume.get_future().share();
    auto handler = std::make_shared<CallbackHandler>();
    handler->callback = [&](WorkJob& job) {
        if (job.type == WorkType::Control) { entered.set_value(); gate.wait(); }
        else { ++handled; }
    };
    ShardedWorkerPool pool;
    CHECK(pool.start(1, handler, 1, policy) == 0);
    WorkJob blocker;
    blocker.type = WorkType::Control;
    CHECK(pool.post(std::move(blocker)) == 0);
    if (started.wait_for(2s) != std::future_status::ready)
    {
        resume.set_value();
        pool.shutdown(false);
        throw std::runtime_error("worker did not enter barrier");
    }
    WorkJob first;
    first.payload = Tracked(released);
    const int accepted = pool.post(std::move(first));
    WorkJob second;
    second.payload = Tracked(released);
    const int overflow = pool.post(std::move(second));
    const int after_overflow = released.load();
    if (drain)
    {
        resume.set_value();
        pool.shutdown(true);
    }
    else
    {
        std::thread stopper([&] { pool.shutdown(false); });
        // Wait for shutdown to discard the queued job before unblocking work.
        const auto deadline = std::chrono::steady_clock::now() + 2s;
        while (released != 2 && std::chrono::steady_clock::now() < deadline)
            std::this_thread::yield();
        const bool discarded = released == 2;
        resume.set_value();
        stopper.join();
        CHECK(discarded);
    }
    CHECK(accepted == 0);
    CHECK((overflow == 0) == (policy == ShardedWorkerPool::DropPolicy::DropHead));
    CHECK(after_overflow == 1);
    CHECK(released == 2);
    CHECK(handled == (drain ? 1 : 0));
}

void QueueByteBudgets(ShardedWorkerPool::DropPolicy policy)
{
    common::MemoryPool memory({65536, 65536});
    std::promise<void> entered, resume;
    auto started = entered.get_future();
    auto gate = resume.get_future().share();
    auto handler = std::make_shared<CallbackHandler>();
    handler->callback = [&](WorkJob& job) {
        if (job.type == WorkType::Control) { entered.set_value(); gate.wait(); }
    };
    ShardedWorkerPool pool;
    CHECK(pool.start(1, handler, 100, policy, 128) == 0);
    WorkJob blocker; blocker.type = WorkType::Control;
    pool.post(std::move(blocker));
    started.wait();
    uint8_t bytes[256]{};
    auto post = [&](size_t size, bool slice = false) {
        WorkJob job;
        job.payload = SharedBuffer::Copy(bytes, size, memory);
        if (slice) job.payload = job.payload.Slice(0, 1);
        return pool.post(std::move(job));
    };
    const int first = post(64), second = post(64);
    const int too_large = post(256, true);
    const auto before = pool.Status();
    const int overflow = post(96); // Rounded capacity is 128; must drop two heads.
    const auto after = pool.Status();
    resume.set_value();
    pool.shutdown(true);
    CHECK(first == 0 && second == 0 && too_large != 0);
    CHECK(before.queue_bytes == 128 && before.queue_depth == 2);
    CHECK(after.queue_bytes == 128 && after.max_bytes_seen == 128);
    CHECK((overflow == 0) == (policy == ShardedWorkerPool::DropPolicy::DropHead));
    CHECK(after.queue_depth == (policy == ShardedWorkerPool::DropPolicy::DropHead ? 1 : 2));
    CHECK(memory.Stats().live_allocations == 0 && pool.Status().queue_bytes == 0);
}

void ExceptionsAndMissingEndpointsReleaseJobs()
{
    std::atomic<int> released{0}, handled{0};
    auto handler = std::make_shared<CallbackHandler>();
    handler->callback = [&](WorkJob&) { ++handled; throw std::runtime_error("test"); };
    ShardedWorkerPool pool;
    CHECK(pool.start(1, handler) == 0);
    for (int i = 0; i < 2; ++i)
    {
        WorkJob job;
        job.payload = Tracked(released);
        CHECK(pool.post(std::move(job)) == 0);
    }
    pool.shutdown(true);
    CHECK(handled == 2 && released == 2);

    auto endpoints = std::make_shared<utils::EndpointJobHandler>(&utils::EndpointManager::Instance());
    CHECK(pool.start(1, endpoints) == 0);
    WorkJob missing;
    missing.target_id = 987654;
    missing.payload = Tracked(released);
    CHECK(pool.post(std::move(missing)) == 0);
    pool.shutdown(true);
    CHECK(released == 3);
}

void MediaIngressRetainsBytesAndStreamAffinity()
{
    struct Captured { SharedBuffer payload; uint64_t time, target, key; WorkType type; };
    std::promise<Captured> captured;
    auto received = captured.get_future();
    auto handler = std::make_shared<CallbackHandler>();
    handler->callback = [&](WorkJob& job) {
        captured.set_value({job.payload, job.enqueue_ts, job.target_id, job.key, job.type});
    };
    CHECK(WorkerService::create_pool(POOL_MEDIA, 1, handler) == 0);
    std::vector<uint8_t> bytes(4096, 0x5a);
    bytes[0] = 0x80;
    bytes[1] = 96;
    bytes[8] = 0; bytes[9] = 0; bytes[10] = 0; bytes[11] = 42;
    auto pooled = SharedBuffer::Copy(bytes.data(), bytes.size());
    const auto* allocation = pooled.Data();
    ReceivedMediaPacket packet(MediaPacketType::Rtp, 9, 123, std::move(pooled));
    media::transport::MediaEndpointIngress ingress(77);
    const auto result = ingress.OnMediaPacket(std::move(packet));
    WorkerService::destroy_pool(POOL_MEDIA, true);
    CHECK(result == MediaPacketIngressResult::Accepted);
    CHECK(!packet.IsValid());
    CHECK(received.wait_for(0s) == std::future_status::ready);
    auto job = received.get();
    CHECK(job.payload.Data() == allocation);
    CHECK(job.payload.Size() == 4096 && job.payload.Data()[4095] == 0x5a);
    CHECK(job.time == 123 && job.target == 77 && job.type == WorkType::Rtp);
    CHECK(job.key == media_affinity::MakeStreamKey(77, 42));
}

class FrameSink final : public media::IEncodedFrameSink
{
public:
    media::EncodedFrameEvent received;
    bool SubmitFrame(const media::EncodedFrameEvent& event) override { received = event; return true; }
};

void EncodedFramesShareOwnedSlices()
{
    std::atomic<int> released{0};
    auto frame = std::make_shared<media::EncodedFrame>();
    frame->info.media_type = media::MediaType::Video;
    frame->info.codec = media::CodecType::H264;
    frame->buffer = Tracked(released, 4096).Slice(4, 4092);
    CHECK(frame->Valid() && frame->Size() == 4092);
    auto first = std::make_shared<FrameSink>();
    auto second = std::make_shared<FrameSink>();
    media::EncodedFrameRouter router;
    router.Subscribe(first);
    router.Subscribe(second);
    media::EncodedFrameEvent event;
    event.source.endpoint_id = 1;
    event.frame = frame;
    CHECK(router.Publish(event) == 2);
    CHECK(first->received.frame->Data() == frame->Data());
    CHECK(second->received.frame->Data() == frame->Data());
    frame.reset(); event = {}; first->received = {};
    CHECK(released == 0);
    second->received = {};
    CHECK(released == 1);

    media::AudioDepacketizer depacketizer(media::CodecType::AAC, 44100, 2, "config=1210");
    std::vector<uint8_t> aac{0, 16, 0, 32, 1, 2, 3, 4};
    RtpView view;
    view.payload = aac.data(); view.payload_len = aac.size(); view.marker = true;
    CHECK(depacketizer.Input(view));
    media::EncodedFrame audio;
    CHECK(depacketizer.PopFrame(audio));
    aac.clear(); depacketizer.Reset();
    CHECK(audio.Valid() && audio.Size() == 4);
    CHECK(audio.buffer.ToVector() == (std::vector<uint8_t>{1, 2, 3, 4}));
    CHECK(audio.codec_config.ToVector() == (std::vector<uint8_t>{0x12, 0x10}));
}
void ThrowingWorkerHooksAreContained()
{
    class Hooks final : public IJobHandler
    {
    public:
        std::promise<void> first_tick;
        std::atomic<int> ticks{0}, stops{0}, handled{0};
        void handle(WorkJob&) override { ++handled; }
        void on_worker_tick(size_t) override
        {
            if (ticks++ == 0) first_tick.set_value();
            throw std::bad_alloc();
        }
        void on_worker_stop(size_t) override { ++stops; throw std::bad_alloc(); }
    };
    auto hooks = std::make_shared<Hooks>();
    auto tick = hooks->first_tick.get_future();
    ShardedWorkerPool workers;
    CHECK(workers.start(1, hooks) == 0);
    const auto ready = tick.wait_for(2s);
    WorkJob job; job.type = WorkType::Control;
    const auto posted = workers.post(std::move(job));
    workers.shutdown(true);
    CHECK(ready == std::future_status::ready && posted == 0);
    CHECK(hooks->handled == 1 && hooks->stops == 1);
}

void MediaAllocationPressureAndRecovery()
{
    auto& pool = common::MemoryPool::Default();
    const auto limits = pool.Options();
    CHECK(pool.SetLimits({0, 0}));
    uint8_t rtp[] = {0x80, 96, 0, 1, 0, 0, 0, 1, 0, 0, 0, 1, 0x65, 1, 2};
    ReceivedMediaPacket rejected(MediaPacketType::Rtp, 1, 1, rtp, sizeof(rtp));
    CHECK(!rejected.IsValid());
    RtpPacket parsed;
    CHECK(!parsed.reserve(1500));
    auto packet_objects = std::make_shared<rtsp::RtpPacketPool>(2);
    CHECK(packet_objects->Capacity() == 2 && !packet_objects->Acquire());
    media::AudioDepacketizer audio(media::CodecType::PCMU, 8000);
    H264Depacketizer video;
    RtpView view;
    view.seq = 1; view.ts = 1; view.ssrc = 1; view.marker = true;
    view.payload = rtp + 12; view.payload_len = 3;
    CHECK(!audio.Input(view) && !audio.HasFrame());
    CHECK(!video.input(view) && !video.hasFrame());
    CHECK(pool.SetLimits(limits));
    CHECK(parsed.reserve(1500));
    {
        auto recovered = packet_objects->Acquire();
        CHECK(recovered && packet_objects->Available() == 1);
    }
    CHECK(packet_objects->Available() == 2);
    parsed.setRaw(rtp, sizeof(rtp));
    CHECK(parsed.getSize() == sizeof(rtp) && parsed.getData()[14] == 2);
    const auto* address = parsed.getData();
    CHECK(!parsed.reserve(std::numeric_limits<size_t>::max()));
    CHECK(parsed.getData() == address && parsed.getSize() == sizeof(rtp));
    parsed.setPayload(nullptr, 0);
    CHECK(parsed.getData() == address);
    parsed.setRaw(rtp, sizeof(rtp));
    Throws<std::out_of_range>([&] { parsed.setPayload(rtp, std::numeric_limits<size_t>::max()); });
    CHECK(parsed.getData()[14] == 2);
    CHECK(audio.Input(view) && audio.HasFrame());
    CHECK(video.input(view) && video.hasFrame());
    auto annexb = video.popFrame();
    CHECK(annexb == (common::ByteVector{0, 0, 0, 1, 0x65, 1, 2}));
}

} // namespace

int main()
{
    BufferOwnershipAndBounds();
    LastOwnerCanOutliveProducerAndPoolHandle();
    RejectedJobsReleaseImmediately();
    QueueDropsReleaseExactlyOnce(ShardedWorkerPool::DropPolicy::DropHead, true);
    QueueDropsReleaseExactlyOnce(ShardedWorkerPool::DropPolicy::DropTail, true);
    QueueDropsReleaseExactlyOnce(ShardedWorkerPool::DropPolicy::DropTail, false);
    QueueByteBudgets(ShardedWorkerPool::DropPolicy::DropHead);
    QueueByteBudgets(ShardedWorkerPool::DropPolicy::DropTail);
    ExceptionsAndMissingEndpointsReleaseJobs();
    MediaIngressRetainsBytesAndStreamAffinity();
    EncodedFramesShareOwnedSlices();
    ThrowingWorkerHooksAreContained();
    MediaAllocationPressureAndRecovery();
    std::cout << "13 memory ownership scenarios passed\n";
}
