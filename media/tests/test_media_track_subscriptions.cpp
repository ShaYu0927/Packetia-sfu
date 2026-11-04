#include <gtest/gtest.h>
#include "EncodedFrameRouter.h"
#include "MediaStreamRegistry.h"
#include "MediaTrackEvent.h"
#include <thread>

namespace
{
media::MediaStream::Ptr Publish(media::MediaStreamRegistry& registry, const std::string& name = "camera")
{
    auto stream = std::make_shared<media::MediaStream>(name);
    EXPECT_TRUE(stream->ReplaceTracks({{0, media::MediaType::Video, media::CodecType::H264},
        {1, media::MediaType::Audio, media::CodecType::Opus, 48000, 2}}));
    EXPECT_TRUE(stream->BindTrackEndpoint(0, 10));
    EXPECT_TRUE(stream->BindTrackEndpoint(1, 10));
    EXPECT_TRUE(stream->StartPublishing());
    EXPECT_TRUE(registry.Register(stream));
    return stream;
}

class Counter final : public media::IEncodedFrameSink
{
public:
    bool SubmitFrame(const media::EncodedFrameEvent&) override { ++calls; return true; }
    unsigned calls = 0;
};

struct OnDestroy
{
    explicit OnDestroy(std::function<void()> callback) : callback(std::move(callback)) {}
    ~OnDestroy() { callback(); }
    std::function<void()> callback;
};

media::EncodedFrameEvent Frame(media::MediaTrackRef ref)
{
    auto frame = std::make_shared<media::EncodedFrame>();
    frame->info.track_id = ref.track_id;
    frame->info.media_type = media::MediaType::Video;
    frame->info.codec = media::CodecType::H264;
    frame->buffer = common::SharedBuffer::Take(std::vector<uint8_t>{0x65, 1});
    media::EncodedFrameEvent event;
    event.source.endpoint_id = 10;
    event.source.media_stream_id = ref.stream_id;
    event.source.track_id = ref.track_id;
    event.frame = std::move(frame);
    return event;
}
}

TEST(MediaTrackSubscriptions, SourceAssignsReferencesAndClonesReceiveNewIdentity)
{
    media::MediaStreamRegistry registry;
    auto stream = Publish(registry);
    const auto tracks = stream->GetTracks();
    ASSERT_EQ(tracks.size(), 2U);
    EXPECT_EQ(tracks[0].ref, (media::MediaTrackRef{stream->GetId(), 0}));
    EXPECT_EQ(registry.FindTrack(tracks[0].ref)->description.ref, tracks[0].ref);
    auto clone = std::make_shared<media::MediaStream>("clone");
    ASSERT_TRUE(clone->ReplaceTracks(tracks));
    EXPECT_NE(clone->GetTracks()[0].ref, tracks[0].ref);
    EXPECT_EQ(clone->GetTracks()[0].ref.stream_id, clone->GetId());
    EXPECT_FALSE(stream->ReplaceTracks(tracks));
}

TEST(MediaTrackSubscriptions, SourceAndIndividualPausesAreIndependentAndKeepBindings)
{
    media::MediaStreamRegistry registry;
    auto stream = Publish(registry);
    const auto video = stream->GetTracks()[0].ref, audio = stream->GetTracks()[1].ref;
    auto b = registry.AttachConsumer(video, "B"), c = registry.AttachConsumer(video, "C");
    auto mic = registry.AttachConsumer(audio, "microphone");
    ASSERT_TRUE(b && c && mic);
    EXPECT_EQ(registry.Size(), 1U);
    EXPECT_EQ(registry.GetSnapshot().tracks.size(), 2U);
    EXPECT_EQ(registry.FindTrack(video)->consumers.size(), 2U);
    ASSERT_TRUE(registry.SetConsumerPaused(b->Id(), true));
    EXPECT_FALSE(b->CanDeliver());
    EXPECT_TRUE(c->CanDeliver());
    ASSERT_TRUE(registry.SetTrackPaused(video, true));
    ASSERT_TRUE(registry.SetConsumerPaused(b->Id(), false));
    EXPECT_FALSE(b->CanDeliver());
    EXPECT_FALSE(c->CanDeliver());
    EXPECT_TRUE(mic->CanDeliver());
    EXPECT_EQ(registry.FindTrack(video)->state, media::MediaTrackState::Paused);
    ASSERT_TRUE(registry.SetTrackPaused(video, false));
    EXPECT_TRUE(b->CanDeliver());
    EXPECT_TRUE(c->CanDeliver());
    c.reset();
    EXPECT_EQ(registry.FindTrack(video)->consumers.size(), 1U);
    EXPECT_TRUE(registry.DetachConsumer(b->Id()));
    EXPECT_FALSE(registry.DetachConsumer(b->Id()));
    EXPECT_TRUE(b->Closed());
}

TEST(MediaTrackSubscriptions, AnnouncedTracksBecomeConsumableOnlyAfterEndpointBindingAndPublishing)
{
    media::MediaStreamRegistry registry;
    auto stream = std::make_shared<media::MediaStream>("announced");
    ASSERT_TRUE(stream->ReplaceTracks({{0, media::MediaType::Video, media::CodecType::H264}}));
    const auto ref = stream->GetTracks()[0].ref;
    ASSERT_TRUE(registry.Register(stream));
    EXPECT_TRUE(registry.FindTrack(ref));
    EXPECT_FALSE(registry.AttachConsumer(ref, "early"));
    ASSERT_TRUE(stream->BindTrackEndpoint(0, 10));
    EXPECT_FALSE(registry.AttachConsumer(ref, "early"));
    ASSERT_TRUE(stream->StartPublishing());
    auto consumer = registry.AttachConsumer(ref, "ready");
    ASSERT_TRUE(consumer);
    EXPECT_TRUE(consumer->CanDeliver());
    EXPECT_EQ(registry.FindTrack(ref)->endpoint_id, 10U);
    EXPECT_FALSE(registry.AttachConsumer(ref, ""));
}

TEST(MediaTrackSubscriptions, RemoveTrackRevokesBeforeNotifyingAndLeavesOtherSourcesAvailable)
{
    media::MediaStreamRegistry registry;
    auto stream = Publish(registry);
    const auto video = stream->GetTracks()[0].ref, audio = stream->GetTracks()[1].ref;
    unsigned closed = 0, events = 0;
    media::MediaTrackSubscription::Ptr consumer;
    consumer = registry.AttachConsumer(video, "recorder", [&] {
        ++closed;
        EXPECT_TRUE(consumer->Closed());
        EXPECT_FALSE(registry.FindTrack(video)); // 回调可重入查询，无注册表锁。
        EXPECT_TRUE(registry.FindTrack(audio));
    });
    auto observer = registry.Events().Subscribe<media::MediaTrackStateEvent>([&](const auto& event) {
        EXPECT_EQ(event.track, video);
        EXPECT_EQ(event.state, media::MediaTrackState::Closed);
        ++events;
    });
    ASSERT_TRUE(registry.RemoveTrack(video));
    EXPECT_EQ(closed, 1U);
    EXPECT_EQ(events, 1U);
    EXPECT_FALSE(registry.RemoveTrack(video));
    EXPECT_FALSE(registry.SetTrackPaused(video, false));
    EXPECT_FALSE(registry.AttachConsumer(video, "late"));
    EXPECT_EQ(stream->GetTracks().size(), 1U);
    EXPECT_TRUE(registry.FindTrack(audio));
    EXPECT_EQ(registry.GetSnapshot().tracks.size(), 1U);
}

TEST(MediaTrackSubscriptions, UnregisterClearAndRepublishFenceOldHandlesAndIsolateCallbacks)
{
    media::MediaStreamRegistry registry;
    auto first = Publish(registry);
    const auto ref = first->GetTracks()[0].ref;
    auto throwing = registry.AttachConsumer(ref, "throw", [] { throw 1; });
    unsigned calls = 0;
    auto consumer = registry.AttachConsumer(ref, "B", [&] { ++calls; });
    ASSERT_TRUE(registry.Remove(first));
    EXPECT_TRUE(throwing->Closed());
    EXPECT_TRUE(consumer->Closed());
    EXPECT_EQ(calls, 1U);
    auto next = Publish(registry);
    auto replacement = registry.AttachConsumer(next->GetTracks()[0].ref, "B", [&] { ++calls; });
    ASSERT_TRUE(replacement);
    EXPECT_FALSE(registry.Remove(first));
    EXPECT_FALSE(registry.DetachConsumer(consumer->Id()));
    EXPECT_TRUE(replacement->CanDeliver());
    registry.Clear();
    registry.Clear();
    EXPECT_TRUE(replacement->Closed());
    EXPECT_EQ(calls, 2U);
}

TEST(MediaTrackSubscriptions, AttachIsBoundedAndExpiredHandlesDoNotConsumeCapacity)
{
    media::MediaStreamRegistry registry;
    auto stream = Publish(registry);
    const auto ref = stream->GetTracks()[0].ref;
    std::vector<media::MediaTrackSubscription::Ptr> consumers;
    for (unsigned i = 0; i < 4096; ++i)
    {
        auto consumer = registry.AttachConsumer(ref, "consumer");
        ASSERT_TRUE(consumer);
        consumers.push_back(std::move(consumer));
    }
    EXPECT_FALSE(registry.AttachConsumer(ref, "overflow"));
    consumers.clear();
    EXPECT_TRUE(registry.FindTrack(ref)->consumers.empty());
    auto next = registry.AttachConsumer(ref, "next");
    EXPECT_TRUE(next);
}

TEST(MediaTrackSubscriptions, ManagedFrameRoutesFollowGlobalControlsWithoutRegisteringCopies)
{
    media::MediaStreamRegistry registry;
    auto stream = Publish(registry);
    const auto ref = stream->GetTracks()[0].ref;
    media::EncodedFrameRouter router;
    auto b = std::make_shared<Counter>(), c = std::make_shared<Counter>();
    unsigned closed = 0;
    const auto subscription = router.Subscribe(b, ref, registry, "B", [&] { ++closed; });
    ASSERT_NE(subscription, 0U);
    ASSERT_NE(router.Subscribe(c, ref, registry, "C"), 0U);
    EXPECT_EQ(router.Publish(Frame(ref)), 2U);
    const auto consumers = registry.FindTrack(ref)->consumers;
    ASSERT_EQ(consumers.size(), 2U);
    ASSERT_TRUE(registry.SetConsumerPaused(consumers[0].id, true));
    EXPECT_EQ(router.Publish(Frame(ref)), 1U);
    EXPECT_EQ(b->calls, 1U);
    EXPECT_EQ(c->calls, 2U);
    ASSERT_TRUE(registry.SetTrackPaused(ref, true));
    EXPECT_EQ(router.Publish(Frame(ref)), 0U);
    ASSERT_TRUE(registry.SetTrackPaused(ref, false));
    EXPECT_EQ(router.Publish(Frame(ref)), 1U);
    router.Unsubscribe(subscription);
    EXPECT_EQ(registry.FindTrack(ref)->consumers.size(), 1U);
    ASSERT_TRUE(registry.Remove(stream));
    EXPECT_EQ(router.Publish(Frame(ref)), 0U);
    EXPECT_EQ(closed, 0U); // 主动退订后不再接收源关闭回调。
    EXPECT_EQ(registry.Size(), 0U);
}

TEST(MediaTrackSubscriptions, ConcurrentAttachAndSourceRemovalCannotLeaveAnOpenHandle)
{
    media::MediaStreamRegistry registry;
    auto stream = Publish(registry);
    const auto ref = stream->GetTracks()[0].ref;
    std::atomic<bool> started{false};
    std::vector<media::MediaTrackSubscription::Ptr> consumers;
    std::thread attach([&] {
        started.store(true);
        for (unsigned i = 0; i < 200; ++i)
            if (auto consumer = registry.AttachConsumer(ref, "consumer")) consumers.push_back(std::move(consumer));
    });
    while (!started.load()) std::this_thread::yield();
    ASSERT_TRUE(registry.Remove(stream));
    attach.join();
    for (const auto& consumer : consumers) EXPECT_TRUE(consumer->Closed());
    EXPECT_FALSE(registry.AttachConsumer(ref, "late"));
}

TEST(MediaTrackSubscriptions, RouterAdmissionFailureRollsBackOnlyTheNewConsumer)
{
    using Status = media::EncodedFrameRouter::SubscribeStatus;
    media::MediaStreamRegistry registry;
    auto stream = Publish(registry);
    const auto ref = stream->GetTracks()[0].ref;
    auto sink = std::make_shared<Counter>();
    unsigned closed = 0;
    media::EncodedFrameRouter router({1});
    ASSERT_TRUE(router.TrySubscribe(sink, ref, registry, "existing", [&] { ++closed; }));
    for (unsigned i = 0; i < 10; ++i)
        EXPECT_EQ(router.TrySubscribe(sink, ref, registry, "overflow", [&] { ++closed; }).status, Status::Full);
    EXPECT_EQ(registry.FindTrack(ref)->consumers.size(), 1U);
    EXPECT_EQ(router.Publish(Frame(ref)), 1U);
    EXPECT_EQ(router.TrySubscribe(sink, ref, registry, "").status, Status::Invalid);
    EXPECT_EQ(router.TrySubscribe(sink, {0, 0}, registry, "invalid").status, Status::Invalid);
    EXPECT_EQ(router.TrySubscribe(sink, {stream->GetId(), 99}, registry, "absent").status, Status::SourceRejected);
    router.Close();
    EXPECT_TRUE(registry.FindTrack(ref)->consumers.empty());
    EXPECT_EQ(router.TrySubscribe(sink, ref, registry, "closed", [&] { ++closed; }).status, Status::Closed);
    EXPECT_TRUE(registry.FindTrack(ref)->consumers.empty());
    EXPECT_TRUE(registry.Remove(stream));
    EXPECT_EQ(closed, 0U);
}

TEST(MediaTrackSubscriptions, RetiredCallbackCapturesAreDestroyedOutsideRouterLock)
{
    media::MediaStreamRegistry registry;
    auto stream = Publish(registry);
    const auto ref = stream->GetTracks()[0].ref;
    media::EncodedFrameRouter router({1});
    auto sink = std::make_shared<Counter>();
    unsigned destroyed = 0;
    const auto subscribe = [&] {
        auto marker = std::make_shared<OnDestroy>([&] {
            ++destroyed;
            router.GetStats();
            router.Unsubscribe(0);
        });
        return router.TrySubscribe(sink, ref, registry, "consumer", [marker] {});
    };
    const auto first = subscribe();
    ASSERT_TRUE(first);
    router.Unsubscribe(first.id);
    EXPECT_EQ(destroyed, 1U);
    ASSERT_TRUE(subscribe());
    sink.reset();
    sink = std::make_shared<Counter>();
    ASSERT_TRUE(subscribe()); // 清理 sink 已过期的旧槽及快照。
    EXPECT_EQ(destroyed, 2U);
    EXPECT_FALSE(subscribe()); // 满额失败也在写锁外释放回调捕获。
    EXPECT_EQ(destroyed, 3U);
    router.Close();
    EXPECT_EQ(destroyed, 4U);
    EXPECT_FALSE(subscribe());
    EXPECT_EQ(destroyed, 5U);
}

TEST(MediaTrackSubscriptions, RouterDestructionClosesManagedConsumersWithoutSourceNotification)
{
    media::MediaStreamRegistry registry;
    auto stream = Publish(registry);
    const auto ref = stream->GetTracks()[0].ref;
    auto sink = std::make_shared<Counter>();
    unsigned closed = 0;
    {
        media::EncodedFrameRouter router;
        ASSERT_TRUE(router.TrySubscribe(sink, ref, registry, "recorder", [&] { ++closed; }));
        ASSERT_EQ(registry.FindTrack(ref)->consumers.size(), 1U);
    }
    EXPECT_TRUE(registry.FindTrack(ref)->consumers.empty());
    EXPECT_TRUE(registry.Remove(stream));
    EXPECT_EQ(closed, 0U);
}
