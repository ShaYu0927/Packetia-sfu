#include <gtest/gtest.h>
#include "EventBus.h"
#include "TrackPublicationEvent.h"
#include <stdexcept>

TEST(EventBus, SubscriptionCanOutliveBus)
{
    media::EventBus::Subscription subscription;
    {
        media::EventBus bus;
        subscription = bus.Subscribe<media::TrackPublishedEvent>([](const auto&) {});
        EXPECT_TRUE(subscription.Valid());
    }
    EXPECT_FALSE(subscription.Valid());
    subscription.Reset();
    subscription.Reset();
}

TEST(EventBus, ResetSkipsCallbackAlreadyCopiedIntoDispatchSnapshot)
{
    media::EventBus bus;
    media::EventBus::Subscription second;
    int calls = 0;
    auto first = bus.Subscribe<media::TrackPublishedEvent>([&](const auto&) { second.Reset(); });
    second = bus.Subscribe<media::TrackPublishedEvent>([&](const auto&) { ++calls; });
    const auto result = bus.EmplacePublish<media::TrackPublishedEvent>(media::TrackPublicationSnapshot{});
    EXPECT_EQ(result.delivered, 1U);
    EXPECT_EQ(result.failed, 0U);
    EXPECT_EQ(calls, 0);
}

TEST(EventBus, ThrowingObserverDoesNotPreventOtherObservers)
{
    media::EventBus bus;
    int calls = 0;
    auto throwing = bus.Subscribe<media::TrackPublishedEvent>([](const auto&) { throw std::runtime_error("observer"); });
    auto healthy = bus.Subscribe<media::TrackPublishedEvent>([&](const auto&) { ++calls; });
    const auto result = bus.EmplacePublish<media::TrackPublishedEvent>(media::TrackPublicationSnapshot{});
    EXPECT_EQ(result.delivered, 1U);
    EXPECT_EQ(result.failed, 1U);
    EXPECT_EQ(calls, 1);
}

TEST(EventBus, SubscriptionCanResetInsideCallback)
{
    media::EventBus bus;
    auto owner = std::make_shared<int>(0);
    const std::weak_ptr<int> weak = owner;
    media::EventBus::Subscription subscription;
    subscription = bus.Subscribe<media::TrackPublishedEvent>([weak, &subscription](const auto&) {
        if (auto current = weak.lock()) ++*current;
        subscription.Reset();
    });
    EXPECT_EQ(bus.EmplacePublish<media::TrackPublishedEvent>(media::TrackPublicationSnapshot{}).delivered, 1U);
    EXPECT_EQ(*owner, 1);
    EXPECT_FALSE(subscription.Valid());
    owner.reset();
    EXPECT_EQ(bus.EmplacePublish<media::TrackPublishedEvent>(media::TrackPublicationSnapshot{}).delivered, 0U);
}

TEST(EventBus, ExpiredOwnerIsNotAccessedByRemainingSubscription)
{
    media::EventBus bus;
    auto owner = std::make_shared<int>(0);
    const std::weak_ptr<int> weak = owner;
    int calls = 0;
    auto subscription = bus.Subscribe<media::TrackPublishedEvent>([weak, &calls](const auto&) {
        if (const auto current = weak.lock()) { ++*current; ++calls; }
    });
    owner.reset();
    EXPECT_EQ(bus.EmplacePublish<media::TrackPublishedEvent>(media::TrackPublicationSnapshot{}).delivered, 1U);
    EXPECT_EQ(calls, 0);
}

TEST(EventBus, DestroyingCallbackCapturesCanUnsubscribeWithoutDeadlock)
{
    media::EventBus::Subscription outer;
    {
        media::EventBus bus;
        auto nested = std::make_shared<media::EventBus::Subscription>();
        *nested = bus.Subscribe<media::TrackUnpublishedEvent>([](const auto&) {});
        outer = bus.Subscribe<media::TrackPublishedEvent>([nested](const auto&) {});
        nested.reset(); // Bus 成为嵌套订阅句柄的最后一个持有者。
    }
    EXPECT_FALSE(outer.Valid());
}
