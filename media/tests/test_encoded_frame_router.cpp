#include <gtest/gtest.h>

#include "EncodedFrameRouter.h"
#include "MediaStreamRegistry.h"
#include <functional>
#include <future>
#include <stdexcept>
#include <thread>

namespace
{
media::EncodedFrameEvent Frame(uint32_t stream = 1, media::TrackId track = 0, uint64_t endpoint = 10)
{
    auto frame = std::make_shared<media::EncodedFrame>();
    frame->info.track_id = track;
    frame->info.media_type = media::MediaType::Video;
    frame->info.codec = media::CodecType::H264;
    frame->buffer = common::SharedBuffer::Take(std::vector<uint8_t>{0x65, 1});
    media::EncodedFrameEvent event;
    event.source.endpoint_id = endpoint;
    event.source.media_stream_id = stream;
    event.source.track_id = track;
    event.source.session_id = "meeting";
    event.source.stream_id = "camera";
    event.source.ssrc = 123;
    event.frame = std::move(frame);
    return event;
}

class Sink final : public media::IEncodedFrameSink
{
public:
    bool SubmitFrame(const media::EncodedFrameEvent& event) override
    {
        ++calls;
        return callback ? callback(event) : true;
    }
    std::atomic<size_t> calls{0};
    std::function<bool(const media::EncodedFrameEvent&)> callback;
};
}

TEST(EncodedFrameRouter, StreamAndTrackSubscriptionsRespectInstanceAndTrackZero)
{
    media::EncodedFrameRouter router;
    auto all = std::make_shared<Sink>(), stream = std::make_shared<Sink>(), track = std::make_shared<Sink>();
    ASSERT_NE(router.Subscribe(all), 0U);
    ASSERT_NE(router.Subscribe(stream, media::EncodedFrameFilter::ForStream(7)), 0U);
    ASSERT_NE(router.Subscribe(track, media::EncodedFrameFilter::ForTrack({7, 0})), 0U);
    EXPECT_EQ(router.Publish(Frame(7, 0)), 3U);
    EXPECT_EQ(router.Publish(Frame(7, 1)), 2U);
    EXPECT_EQ(router.Publish(Frame(8, 0)), 1U);
    EXPECT_EQ(router.Publish(Frame(0, 0)), 1U);
    EXPECT_EQ(all->calls, 4U);
    EXPECT_EQ(stream->calls, 2U);
    EXPECT_EQ(track->calls, 1U);
}

TEST(EncodedFrameRouter, CombinesLegacyAndCoreConditionsWithoutDuplicateDelivery)
{
    media::EncodedFrameRouter router;
    auto sink = std::make_shared<Sink>();
    auto filter = media::EncodedFrameFilter::ForTrack({1, 0});
    filter.endpoint_id = 10;
    filter.session_id = "meeting";
    filter.stream_id = "camera";
    filter.ssrc = 123;
    ASSERT_NE(router.Subscribe(sink, filter), 0U);
    const auto event = Frame();
    sink->callback = [&](const auto& received) {
        EXPECT_EQ(received.frame.get(), event.frame.get());
        return true;
    };
    EXPECT_EQ(router.Publish(event), 1U);
    auto wrong = event;
    wrong.source.session_id = "other";
    EXPECT_EQ(router.Publish(wrong), 0U);
    wrong = event; wrong.source.stream_id = "screen";
    EXPECT_EQ(router.Publish(wrong), 0U);
    wrong = event; wrong.source.ssrc = 456;
    EXPECT_EQ(router.Publish(wrong), 0U);
    wrong = event; wrong.source.endpoint_id = 11;
    EXPECT_EQ(router.Publish(wrong), 0U);
    EXPECT_EQ(sink->calls, 1U);
}

TEST(EncodedFrameRouter, ReportsBackpressureAndIsolatesConsumerExceptions)
{
    media::EncodedFrameRouter router;
    auto accepted = std::make_shared<Sink>(), rejected = std::make_shared<Sink>(), failed = std::make_shared<Sink>();
    rejected->callback = [](const auto&) { return false; };
    failed->callback = [](const auto&) -> bool { throw std::runtime_error("consumer failed"); };
    router.Subscribe(accepted);
    router.Subscribe(rejected);
    router.Subscribe(failed);
    const auto result = router.Dispatch(Frame());
    EXPECT_EQ(result.accepted, 1U);
    EXPECT_EQ(result.rejected, 1U);
    EXPECT_EQ(result.failed, 1U);
    EXPECT_EQ(router.Publish(Frame()), 1U);
    EXPECT_EQ(accepted->calls, 2U);
    EXPECT_EQ(rejected->calls, 2U);
    EXPECT_EQ(failed->calls, 2U);
    auto invalid = Frame(); invalid.frame.reset();
    const auto ignored = router.Dispatch(invalid);
    EXPECT_EQ(ignored.accepted + ignored.rejected + ignored.failed, 0U);
    EXPECT_EQ(accepted->calls, 2U);
    const auto stats = router.GetStats();
    EXPECT_EQ(stats.frames, 2U);
    EXPECT_EQ(stats.invalid_frames, 1U);
    EXPECT_EQ(stats.accepted, 2U);
    EXPECT_EQ(stats.rejected, 2U);
    EXPECT_EQ(stats.failed, 2U);
    EXPECT_EQ(stats.examined, 6U);
    EXPECT_EQ(stats.active_subscriptions, 3U);
}

TEST(EncodedFrameRouter, InvalidTrackSelectorsCannotBecomeBroadcastSubscriptions)
{
    media::EncodedFrameRouter router;
    auto sink = std::make_shared<Sink>();
    media::EncodedFrameFilter filter;
    filter.track_id = 0;
    EXPECT_EQ(router.Subscribe(sink, filter), 0U);
    EXPECT_EQ(router.Subscribe(nullptr), 0U);
    EXPECT_THROW(media::EncodedFrameFilter::ForStream(0), std::invalid_argument);
    EXPECT_THROW(media::EncodedFrameFilter::ForTrack({0, 0}), std::invalid_argument);
    filter.endpoint_id = 10;
    ASSERT_NE(router.Subscribe(sink, filter), 0U);
    EXPECT_EQ(router.Publish(Frame(0, 0, 10)), 1U);
    EXPECT_EQ(router.Publish(Frame(0, 1, 10)), 0U);
    EXPECT_EQ(router.Publish(Frame(0, 0, 11)), 0U);
}

TEST(EncodedFrameRouter, UnsubscribeInvalidatesAnAlreadyCapturedRouteSnapshot)
{
    media::EncodedFrameRouter router;
    auto controller = std::make_shared<Sink>(), target = std::make_shared<Sink>();
    const auto id = router.Subscribe(target, media::EncodedFrameFilter::ForStream(1));
    controller->callback = [&](const auto&) { router.Unsubscribe(id); return true; };
    router.Subscribe(controller);
    // 全量订阅回调先于指定流桶执行；此时 Dispatch 已持有旧快照。
    EXPECT_EQ(router.Publish(Frame()), 1U);
    EXPECT_EQ(target->calls, 0U);
    router.Unsubscribe(id);
    EXPECT_EQ(router.Publish(Frame()), 1U);
}

TEST(EncodedFrameRouter, SubscriptionChangesInsideCallbacksApplyToTheNextFrame)
{
    media::EncodedFrameRouter router;
    auto controller = std::make_shared<Sink>(), target = std::make_shared<Sink>();
    media::EncodedFrameRouter::SubscriptionId next = 0;
    controller->callback = [&](const auto&) {
        if (!next) next = router.Subscribe(target, media::EncodedFrameFilter::ForTrack({1, 0}));
        return true;
    };
    const auto controllerId = router.Subscribe(controller);
    EXPECT_EQ(router.Publish(Frame()), 1U);
    EXPECT_EQ(target->calls, 0U);
    EXPECT_EQ(router.Publish(Frame()), 2U);
    router.Unsubscribe(controllerId);
    controller.reset();
    target.reset();
    EXPECT_EQ(router.Publish(Frame()), 0U);
    auto replacement = std::make_shared<Sink>();
    EXPECT_NE(router.Subscribe(replacement), 0U);
    EXPECT_EQ(router.Publish(Frame()), 1U);
}

TEST(EncodedFrameRouter, RepublishedLogicalStreamRequiresANewInstanceSubscription)
{
    media::MediaStreamRegistry registry;
    media::EncodedFrameRouter router;
    const auto create = [&] {
        auto stream = std::make_shared<media::MediaStream>("camera");
        EXPECT_TRUE(stream->ReplaceTracks({{0, media::MediaType::Video, media::CodecType::H264}}));
        EXPECT_TRUE(stream->BindTrackEndpoint(0, 10));
        EXPECT_TRUE(stream->StartPublishing());
        EXPECT_TRUE(registry.Register(stream));
        return stream;
    };
    auto first = create();
    auto oldConsumer = std::make_shared<Sink>(), newConsumer = std::make_shared<Sink>();
    router.Subscribe(oldConsumer, media::EncodedFrameFilter::ForTrack({first->GetId(), 0}));
    EXPECT_EQ(router.Publish(Frame(first->GetId())), 1U);
    ASSERT_TRUE(registry.Remove(first));
    auto replacement = create();
    const auto resolved = registry.FindTrack({replacement->GetId(), 0});
    ASSERT_TRUE(resolved);
    EXPECT_EQ(resolved->endpoint_id, 10U);
    EXPECT_EQ(router.Publish(Frame(replacement->GetId())), 0U);
    router.Subscribe(newConsumer, media::EncodedFrameFilter::ForTrack(resolved->ref));
    EXPECT_EQ(router.Publish(Frame(replacement->GetId())), 1U);
    EXPECT_EQ(oldConsumer->calls, 1U);
    EXPECT_EQ(newConsumer->calls, 1U);
}

TEST(EncodedFrameRouter, ConcurrentPublishersAndSubscriptionChangesKeepStableConsumers)
{
    media::EncodedFrameRouter router;
    auto stable = std::make_shared<Sink>();
    router.Subscribe(stable, media::EncodedFrameFilter::ForStream(1));
    std::atomic<bool> start{false};
    std::vector<std::thread> workers;
    for (unsigned i = 0; i < 4; ++i)
        workers.emplace_back([&] {
            const auto event = Frame();
            while (!start.load()) std::this_thread::yield();
            for (unsigned frame = 0; frame < 500; ++frame) EXPECT_GE(router.Publish(event), 1U);
        });
    workers.emplace_back([&] {
        while (!start.load()) std::this_thread::yield();
        for (unsigned change = 0; change < 200; ++change)
        {
            auto transient = std::make_shared<Sink>();
            const auto id = router.Subscribe(transient, media::EncodedFrameFilter::ForTrack({1, 0}));
            router.Unsubscribe(id);
        }
    });
    start.store(true);
    for (auto& worker : workers) worker.join();
    EXPECT_EQ(stable->calls, 2000U);
}

TEST(EncodedFrameRouter, AdmissionIsBoundedAndExpiredSinksReleaseCapacity)
{
    using Status = media::EncodedFrameRouter::SubscribeStatus;
    EXPECT_THROW((media::EncodedFrameRouter{media::EncodedFrameRouter::Options{0}}), std::invalid_argument);
    media::EncodedFrameRouter router({2});
    auto stable = std::make_shared<Sink>(), temporary = std::make_shared<Sink>();
    const auto first = router.TrySubscribe(stable);
    ASSERT_TRUE(first);
    ASSERT_TRUE(router.TrySubscribe(temporary));
    const auto full = router.TrySubscribe(stable);
    EXPECT_EQ(full.status, Status::Full);
    EXPECT_EQ(full.id, 0U);
    EXPECT_FALSE(full);
    EXPECT_EQ(router.TrySubscribe(nullptr).status, Status::Invalid);
    media::EncodedFrameFilter invalid;
    invalid.track_id = 0;
    EXPECT_EQ(router.TrySubscribe(stable, invalid).status, Status::Invalid);
    temporary.reset();
    EXPECT_EQ(router.GetStats().active_subscriptions, 1U);
    EXPECT_TRUE(router.TrySubscribe(stable));
    EXPECT_EQ(router.GetStats().active_subscriptions, 2U);
    EXPECT_EQ(router.GetStats().max_subscriptions, 2U);
    router.Unsubscribe(first.id);
    const auto replacement = router.TrySubscribe(stable);
    EXPECT_TRUE(replacement);
    EXPECT_NE(replacement.id, first.id);
    EXPECT_EQ(router.Publish(Frame()), 2U);
}

TEST(EncodedFrameRouter, ExactTrackIndexAvoidsExaminingUnrelatedTracks)
{
    media::EncodedFrameRouter router;
    auto sink = std::make_shared<Sink>();
    ASSERT_TRUE(router.TrySubscribe(sink));
    ASSERT_TRUE(router.TrySubscribe(sink, media::EncodedFrameFilter::ForStream(7)));
    media::EncodedFrameFilter endpoint;
    endpoint.endpoint_id = 10;
    ASSERT_TRUE(router.TrySubscribe(sink, endpoint));
    for (media::TrackId track = 0; track < 256; ++track)
        ASSERT_TRUE(router.TrySubscribe(sink, media::EncodedFrameFilter::ForTrack({7, track})));
    ASSERT_TRUE(router.TrySubscribe(sink, media::EncodedFrameFilter::ForTrack({8, 0})));
    endpoint.endpoint_id = 11;
    ASSERT_TRUE(router.TrySubscribe(sink, endpoint));
    const auto result = router.Dispatch(Frame(7, 0));
    EXPECT_EQ(result.examined, 4U);
    EXPECT_EQ(result.accepted, 4U);
    EXPECT_EQ(sink->calls, 4U);
    EXPECT_EQ(router.GetStats().active_subscriptions, 261U);
}

TEST(EncodedFrameRouter, CloseInsideCallbackInvalidatesCapturedRoutesAndIsIdempotent)
{
    using Status = media::EncodedFrameRouter::SubscribeStatus;
    media::EncodedFrameRouter router;
    auto controller = std::make_shared<Sink>(), target = std::make_shared<Sink>();
    controller->callback = [&](const auto&) {
        EXPECT_EQ(router.GetStats().active_subscriptions, 2U);
        router.Close();
        EXPECT_TRUE(router.GetStats().closed);
        return true;
    };
    ASSERT_TRUE(router.TrySubscribe(controller));
    ASSERT_TRUE(router.TrySubscribe(target, media::EncodedFrameFilter::ForTrack({1, 0})));
    EXPECT_EQ(router.Publish(Frame()), 1U);
    EXPECT_EQ(target->calls, 0U);
    router.Close();
    router.Unsubscribe(1);
    EXPECT_EQ(router.TrySubscribe(target).status, Status::Closed);
    EXPECT_EQ(router.Subscribe(target), 0U);
    EXPECT_EQ(router.Publish(Frame()), 0U);
    EXPECT_EQ(router.GetStats().active_subscriptions, 0U);
    EXPECT_EQ(router.GetStats().frames, 1U);
    EXPECT_EQ(router.GetStats().accepted, 1U);
}

TEST(EncodedFrameRouter, CloseRejectsNewWorkWhileAnExistingCallbackFinishes)
{
    using Status = media::EncodedFrameRouter::SubscribeStatus;
    media::EncodedFrameRouter router;
    auto sink = std::make_shared<Sink>();
    std::promise<void> entered, release;
    auto released = release.get_future();
    sink->callback = [&](const auto&) {
        entered.set_value();
        released.wait();
        return true;
    };
    ASSERT_TRUE(router.TrySubscribe(sink));
    std::thread publisher([&] { EXPECT_EQ(router.Publish(Frame()), 1U); });
    entered.get_future().wait();
    // 关闭不等待正在执行的消费者；后续分发及并发订阅立即被拒绝。
    router.Close();
    std::thread subscriber([&] {
        for (unsigned i = 0; i < 100; ++i)
            EXPECT_EQ(router.TrySubscribe(sink).status, Status::Closed);
    });
    EXPECT_EQ(router.Publish(Frame()), 0U);
    subscriber.join();
    release.set_value();
    publisher.join();
    EXPECT_EQ(sink->calls, 1U);
    EXPECT_EQ(router.GetStats().accepted, 1U);
}
