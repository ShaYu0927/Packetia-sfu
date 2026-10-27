#include <gtest/gtest.h>
#include "Room.h"
#include "Participant.h"
#include "MediaTrack.h"

namespace
{
media::MediaTrackPtr Camera()
{
    media::TrackMetadata metadata;
    metadata.sid = "camera";
    metadata.type = media::MediaType::Video;
    return std::make_shared<media::MediaTrack>(metadata);
}
}

TEST(RoomEvents, NotifyOnlyCommittedChangesAndCallbacksCanQueryRoom)
{
    room::RoomOptions options;
    options.auto_subscribe = false;
    room::Room room({"events", "events"}, options);
    auto publisher = std::make_shared<room::Participant>("a", "Alice");
    auto viewer = std::make_shared<room::Participant>("b", "Bob");
    ASSERT_TRUE(room.Join(publisher));
    ASSERT_TRUE(room.Join(viewer));
    std::vector<media::TrackPublicationSnapshot> published, removed;
    auto onPublish = room.Events().Subscribe<media::TrackPublishedEvent>([&](const auto& event) {
        EXPECT_EQ(room.ParticipantCount(), 2U); // 若仍持有房间锁，这里会死锁。
        EXPECT_TRUE(publisher->GetPublishedTrack(event.snapshot.track.sid));
        published.push_back(event.snapshot);
    });
    auto onRemove = room.Events().Subscribe<media::TrackUnpublishedEvent>([&](const auto& event) {
        EXPECT_TRUE(room.GetSubscriptionsByTrack(event.snapshot.track.sid).empty());
        EXPECT_FALSE(publisher->GetPublishedTrack(event.snapshot.track.sid));
        removed.push_back(event.snapshot);
    });
    EXPECT_FALSE(room.PublishTrack("missing", Camera(), 0, 96));
    ASSERT_TRUE(room.PublishTrack("a", Camera(), 0, 96));
    EXPECT_FALSE(room.PublishTrack("a", Camera(), 0, 96));
    ASSERT_EQ(published.size(), 1U);
    ASSERT_TRUE(room.SubscribeTrack("b", "camera"));
    ASSERT_TRUE(room.UnpublishTrack("camera"));
    EXPECT_FALSE(room.UnpublishTrack("camera"));
    ASSERT_EQ(removed.size(), 1U);
    EXPECT_EQ(removed[0].publication_id, published[0].publication_id);
    EXPECT_GT(removed[0].revision, published[0].revision);
    EXPECT_EQ(published[0].room_id, "events");
    EXPECT_EQ(published[0].publisher_id, "a");
    EXPECT_FALSE(viewer->HasSubscribedTrack("camera"));
    ASSERT_TRUE(room.PublishTrack("a", Camera(), 0, 96));
    ASSERT_EQ(published.size(), 2U);
    EXPECT_NE(published[1].publication_id, published[0].publication_id);
    ASSERT_TRUE(room.Leave("a"));
    ASSERT_EQ(removed.size(), 2U);
    EXPECT_EQ(removed[1].publication_id, published[1].publication_id);
    room.Close();
    room.Close();
    EXPECT_EQ(removed.size(), 2U);
}

TEST(RoomEvents, CloseRemovesEveryPublicationOnceAndObserverCanOutliveRoom)
{
    media::EventBus::Subscription subscription;
    std::vector<media::TrackPublicationSnapshot> removed;
    {
        room::Room room({"close-events", "close-events"});
        auto publisher = std::make_shared<room::Participant>("a", "Alice");
        ASSERT_TRUE(room.Join(publisher));
        ASSERT_TRUE(room.PublishTrack("a", Camera(), 0, 96));
        subscription = room.Events().Subscribe<media::TrackUnpublishedEvent>([&](const auto& event) {
            EXPECT_TRUE(room.IsClosed());
            EXPECT_EQ(room.ParticipantCount(), 0U);
            EXPECT_TRUE(publisher->GetPublishedTracks().empty());
            removed.push_back(event.snapshot);
        });
        room.Close();
        room.Close();
        EXPECT_EQ(removed.size(), 1U);
        EXPECT_FALSE(room.PublishTrack("a", Camera(), 0, 96));
    }
    EXPECT_FALSE(subscription.Valid());
    subscription.Reset();
}
