#include <gtest/gtest.h>

#include "Participant.h"
#include "SfuEndpoint.h"
#include "MediaTrack.h"

TEST(Participant, StaleSignalingCloseCannotDetachReplacement)
{
    auto participant = std::make_shared<room::Participant>(
        room::ParticipantIdentity{"p-1", "Alice", "user-1"});
    EXPECT_FALSE(participant->BindSignaling(""));
    ASSERT_TRUE(participant->BindSignaling("ws-1"));
    const auto old = participant->GetSignaling();
    ASSERT_TRUE(participant->BindSignaling("ws-2"));
    EXPECT_FALSE(participant->UnbindSignaling(old));
    EXPECT_EQ(participant->GetSignaling().connection_id, "ws-2");
    const auto current = participant->GetSignaling();
    ASSERT_TRUE(participant->UnbindSignaling(current));
    EXPECT_FALSE(participant->UnbindSignaling(current));

    ASSERT_TRUE(participant->BindSignaling("ws-2"));
    EXPECT_FALSE(participant->UnbindSignaling(current));
    EXPECT_GT(participant->GetSignaling().generation, current.generation);
    const auto snapshot = participant->GetInfo();
    participant->SetName("Alice updated");
    EXPECT_EQ(snapshot.identity.name, "Alice");
    EXPECT_EQ(participant->GetInfo().identity.subject_id, "user-1");
    EXPECT_EQ(participant->Id(), "p-1");
}

TEST(Participant, MediaBindingPreservesExistingEndpointAndSession)
{
    auto participant = std::make_shared<room::Participant>("p-1", "Alice");
    auto endpoint = std::make_shared<media::SfuEndpoint>(8001);
    auto session = std::make_shared<room::ParticipantSession>(
        8001, room::ParticipantSessionRole::Publish, endpoint);
    EXPECT_FALSE(participant->BindMediaSession(nullptr));
    EXPECT_FALSE(participant->BindMediaSession(session));
    ASSERT_TRUE(endpoint->Start());
    ASSERT_TRUE(participant->BindEndpoint(endpoint));
    ASSERT_TRUE(participant->BindMediaSession(session));
    ASSERT_TRUE(participant->BindMediaSession(session));
    EXPECT_EQ(participant->GetMediaSession(), session);
    EXPECT_EQ(participant->GetInfo().media_session_id, 8001U);

    auto replacement_endpoint = std::make_shared<media::SfuEndpoint>(8002);
    auto replacement = std::make_shared<room::ParticipantSession>(
        8002, room::ParticipantSessionRole::Publish, replacement_endpoint);
    ASSERT_TRUE(replacement_endpoint->Start());
    EXPECT_FALSE(participant->BindMediaSession(replacement));
    EXPECT_FALSE(participant->BindEndpoint(replacement_endpoint));
    EXPECT_EQ(participant->GetEndpoint(), endpoint);
    EXPECT_EQ(participant->GetMediaSession(), session);
    replacement_endpoint->Stop();
    participant->Leave();
}

TEST(Participant, LeaveDetachesBindingsBeforeCallbackAndCannotRebind)
{
    auto participant = std::make_shared<room::Participant>("p-1", "Alice");
    auto endpoint = std::make_shared<media::SfuEndpoint>(8003);
    ASSERT_TRUE(endpoint->Start());
    auto session = std::make_shared<room::ParticipantSession>(
        8003, room::ParticipantSessionRole::Publish, endpoint);
    ASSERT_TRUE(participant->BindMediaSession(session));
    ASSERT_TRUE(participant->BindSignaling("ws-1"));
    media::TrackInfo track;
    track.sid = "camera";
    ASSERT_TRUE(participant->AddPublishedTrack(std::make_shared<media::MediaTrack>(track)));
    ASSERT_TRUE(participant->SubscribeTrack("remote-camera"));
    std::weak_ptr<const room::ParticipantSession> weak_session = session;
    session.reset();
    int callbacks = 0;
    participant->OnLeave([&](const auto& leaving) {
        ++callbacks;
        const auto info = leaving->GetInfo();
        EXPECT_EQ(info.state, room::ParticipantState::Disconnected);
        EXPECT_TRUE(info.signaling.connection_id.empty());
        EXPECT_EQ(info.media_session_id, 0U);
        EXPECT_EQ(info.published_track_count, 0U);
        EXPECT_EQ(info.subscribed_track_count, 0U);
        EXPECT_FALSE(leaving->GetMediaSession());
        EXPECT_TRUE(weak_session.expired());
        EXPECT_FALSE(leaving->GetEndpoint());
        EXPECT_FALSE(endpoint->IsRunning());
        EXPECT_FALSE(leaving->BindSignaling("ws-2"));
        leaving->Leave();
    });
    participant->Leave();
    participant->Leave();
    EXPECT_EQ(callbacks, 1);
    EXPECT_TRUE(weak_session.expired());

    auto replacement_endpoint = std::make_shared<media::SfuEndpoint>(8004);
    auto replacement = std::make_shared<room::ParticipantSession>(
        8004, room::ParticipantSessionRole::Publish, replacement_endpoint);
    ASSERT_TRUE(replacement_endpoint->Start());
    EXPECT_FALSE(participant->BindMediaSession(replacement));
    replacement_endpoint->Stop();
}

TEST(Participant, PublishAndSubscribeBindingsRemainIndependent)
{
    auto participant = std::make_shared<room::Participant>("p-1", "Alice");
    auto publish_endpoint = std::make_shared<media::SfuEndpoint>(8010);
    auto subscribe_endpoint = std::make_shared<media::SfuEndpoint>(8011);
    ASSERT_TRUE(publish_endpoint->Start());
    ASSERT_TRUE(subscribe_endpoint->Start());
    auto publication = std::make_shared<room::ParticipantSession>(
        8010, room::ParticipantSessionRole::Publish, publish_endpoint);
    auto subscription = std::make_shared<room::ParticipantSession>(
        8011, room::ParticipantSessionRole::Subscribe, subscribe_endpoint);

    ASSERT_TRUE(participant->BindPublishSession(publication));
    const auto publishing_only = participant->GetInfo();
    ASSERT_TRUE(participant->BindSubscribeSession(subscription));
    EXPECT_TRUE(participant->BindPublishSession(publication));
    EXPECT_TRUE(participant->BindSubscribeSession(subscription));
    EXPECT_TRUE(participant->HasMediaBinding());
    EXPECT_EQ(participant->GetPublishSession(), publication);
    EXPECT_EQ(participant->GetSubscribeSession(), subscription);
    EXPECT_EQ(participant->GetPublishEndpoint(), publish_endpoint);
    EXPECT_EQ(participant->GetSubscribeEndpoint(), subscribe_endpoint);
    // The legacy accessors cannot select a role on the caller's behalf.
    EXPECT_FALSE(participant->GetMediaSession());
    EXPECT_FALSE(participant->GetEndpoint());
    const auto duplex = participant->GetInfo();
    EXPECT_EQ(duplex.publish_session_id, 8010U);
    EXPECT_EQ(duplex.subscribe_session_id, 8011U);
    EXPECT_EQ(duplex.media_session_id, 0U);
    EXPECT_EQ(publishing_only.publish_session_id, 8010U);
    EXPECT_EQ(publishing_only.subscribe_session_id, 0U);
    EXPECT_EQ(publishing_only.media_session_id, 8010U);

    auto replacement_endpoint = std::make_shared<media::SfuEndpoint>(8012);
    ASSERT_TRUE(replacement_endpoint->Start());
    auto replacement = std::make_shared<room::ParticipantSession>(
        8012, room::ParticipantSessionRole::Subscribe, replacement_endpoint);
    EXPECT_FALSE(participant->BindSubscribeSession(replacement));
    EXPECT_EQ(participant->GetPublishSession(), publication);
    EXPECT_EQ(participant->GetSubscribeSession(), subscription);
    EXPECT_TRUE(publish_endpoint->IsRunning());
    EXPECT_TRUE(subscribe_endpoint->IsRunning());
    replacement_endpoint->Stop();
    participant->Leave();
}

TEST(Participant, RoleBindingsRejectInvalidSessionsAndSupportSubscribeFirst)
{
    auto participant = std::make_shared<room::Participant>("p-1", "Alice");
    auto publish_endpoint = std::make_shared<media::SfuEndpoint>(8013);
    auto subscribe_endpoint = std::make_shared<media::SfuEndpoint>(8014);
    auto publication = std::make_shared<room::ParticipantSession>(
        8013, room::ParticipantSessionRole::Publish, publish_endpoint);
    auto subscription = std::make_shared<room::ParticipantSession>(
        8014, room::ParticipantSessionRole::Subscribe, subscribe_endpoint);
    EXPECT_FALSE(participant->BindPublishSession(nullptr));
    EXPECT_FALSE(participant->BindSubscribeSession(nullptr));
    EXPECT_FALSE(participant->BindPublishSession(publication));
    EXPECT_FALSE(participant->BindSubscribeSession(subscription));
    ASSERT_TRUE(publish_endpoint->Start());
    ASSERT_TRUE(subscribe_endpoint->Start());
    EXPECT_FALSE(participant->BindPublishSession(subscription));
    EXPECT_FALSE(participant->BindSubscribeSession(publication));
    EXPECT_FALSE(participant->BindPublishSession(std::make_shared<room::ParticipantSession>(
        0, room::ParticipantSessionRole::Publish, publish_endpoint)));
    EXPECT_FALSE(participant->BindSubscribeSession(std::make_shared<room::ParticipantSession>(
        8014, room::ParticipantSessionRole::Subscribe, nullptr)));
    EXPECT_FALSE(participant->HasMediaBinding());
    EXPECT_FALSE(participant->GetPublishSession());
    EXPECT_FALSE(participant->GetSubscribeSession());

    ASSERT_TRUE(participant->BindSubscribeSession(subscription));
    EXPECT_EQ(participant->GetMediaSession(), subscription);
    EXPECT_EQ(participant->GetEndpoint(), subscribe_endpoint);
    EXPECT_FALSE(participant->GetPublishEndpoint());
    // The compatibility API also dispatches a binding to its declared role.
    ASSERT_TRUE(participant->BindMediaSession(publication));
    EXPECT_EQ(participant->GetPublishSession(), publication);
    EXPECT_EQ(participant->GetSubscribeSession(), subscription);
    participant->Leave();
}

TEST(Participant, LeaveDetachesBothRolesAndPreservesPriorSnapshot)
{
    auto participant = std::make_shared<room::Participant>("p-1", "Alice");
    auto publish_endpoint = std::make_shared<media::SfuEndpoint>(8015);
    auto subscribe_endpoint = std::make_shared<media::SfuEndpoint>(8016);
    ASSERT_TRUE(publish_endpoint->Start());
    ASSERT_TRUE(subscribe_endpoint->Start());
    auto publication = std::make_shared<room::ParticipantSession>(
        8015, room::ParticipantSessionRole::Publish, publish_endpoint);
    auto subscription = std::make_shared<room::ParticipantSession>(
        8016, room::ParticipantSessionRole::Subscribe, subscribe_endpoint);
    ASSERT_TRUE(participant->BindPublishSession(publication));
    ASSERT_TRUE(participant->BindSubscribeSession(subscription));
    ASSERT_TRUE(participant->BindSignaling("ws-1"));
    media::TrackInfo track;
    track.sid = "camera";
    ASSERT_TRUE(participant->AddPublishedTrack(std::make_shared<media::MediaTrack>(track)));
    ASSERT_TRUE(participant->SubscribeTrack("remote-camera"));
    const auto active = participant->GetInfo();
    std::weak_ptr<const room::ParticipantSession> weak_publication = publication;
    std::weak_ptr<const room::ParticipantSession> weak_subscription = subscription;
    publication.reset();
    subscription.reset();

    int callbacks = 0;
    participant->OnLeave([&](const auto& leaving) {
        ++callbacks;
        const auto info = leaving->GetInfo();
        EXPECT_EQ(info.state, room::ParticipantState::Disconnected);
        EXPECT_TRUE(info.signaling.connection_id.empty());
        EXPECT_EQ(info.publish_session_id, 0U);
        EXPECT_EQ(info.subscribe_session_id, 0U);
        EXPECT_EQ(info.published_track_count, 0U);
        EXPECT_EQ(info.subscribed_track_count, 0U);
        EXPECT_FALSE(leaving->HasMediaBinding());
        EXPECT_FALSE(leaving->GetPublishSession());
        EXPECT_FALSE(leaving->GetSubscribeSession());
        EXPECT_FALSE(leaving->GetPublishEndpoint());
        EXPECT_FALSE(leaving->GetSubscribeEndpoint());
        EXPECT_TRUE(weak_publication.expired());
        EXPECT_TRUE(weak_subscription.expired());
        EXPECT_FALSE(publish_endpoint->IsRunning());
        EXPECT_FALSE(subscribe_endpoint->IsRunning());
        leaving->Leave();
    });
    participant->Leave();
    participant->Leave();
    EXPECT_EQ(callbacks, 1);
    EXPECT_EQ(active.publish_session_id, 8015U);
    EXPECT_EQ(active.subscribe_session_id, 8016U);
    EXPECT_EQ(active.signaling.connection_id, "ws-1");
    EXPECT_EQ(active.published_track_count, 1U);
    EXPECT_EQ(active.subscribed_track_count, 1U);

    auto replacement_endpoint = std::make_shared<media::SfuEndpoint>(8017);
    ASSERT_TRUE(replacement_endpoint->Start());
    EXPECT_FALSE(participant->BindPublishSession(std::make_shared<room::ParticipantSession>(
        8017, room::ParticipantSessionRole::Publish, replacement_endpoint)));
    EXPECT_FALSE(participant->BindSubscribeSession(std::make_shared<room::ParticipantSession>(
        8017, room::ParticipantSessionRole::Subscribe, replacement_endpoint)));
    replacement_endpoint->Stop();
}

TEST(Participant, FailedMediaAttemptUnbindsOnlyItsExactSession)
{
    auto participant = std::make_shared<room::Participant>("rollback", "rollback");
    auto endpoint = std::make_shared<media::SfuEndpoint>(8020);
    ASSERT_TRUE(endpoint->Start());
    ASSERT_TRUE(participant->BindSignaling("ws-rollback"));
    const auto signaling = participant->GetSignaling();
    auto first = std::make_shared<room::ParticipantSession>(
        8020, room::ParticipantSessionRole::Publish, endpoint);
    auto second = std::make_shared<room::ParticipantSession>(
        8021, room::ParticipantSessionRole::Publish, endpoint);
    ASSERT_TRUE(participant->BindMediaSession(first));
    EXPECT_FALSE(participant->UnbindMediaSession(second));
    ASSERT_TRUE(participant->UnbindMediaSession(first));
    EXPECT_FALSE(participant->UnbindMediaSession(first));
    ASSERT_TRUE(participant->BindMediaSession(second));
    EXPECT_FALSE(participant->UnbindMediaSession(first));
    EXPECT_EQ(participant->GetPublishSession(), second);
    EXPECT_EQ(participant->GetSignaling().connection_id, signaling.connection_id);
    EXPECT_EQ(participant->GetSignaling().generation, signaling.generation);
    EXPECT_TRUE(endpoint->IsRunning());
    participant->Leave();
}
