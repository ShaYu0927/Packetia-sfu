#include <gtest/gtest.h>

#include "EncodedFrameRouter.h"
#include "MediaStreamRegistry.h"
#include <functional>
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
