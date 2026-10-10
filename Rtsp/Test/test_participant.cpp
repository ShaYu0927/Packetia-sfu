#include <gtest/gtest.h>

#include "Participant.h"
#include "media/endpoint/MediaEndpoint.h"

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
    auto session = std::make_shared<room::ParticipantSession>();
    session->id = 8001;
    session->endpoint = endpoint;
    EXPECT_FALSE(participant->BindMediaSession(nullptr));
    EXPECT_FALSE(participant->BindMediaSession(session));
    ASSERT_TRUE(endpoint->Start());
    ASSERT_TRUE(participant->BindEndpoint(endpoint));
    ASSERT_TRUE(participant->BindMediaSession(session));
    ASSERT_TRUE(participant->BindMediaSession(session));
    EXPECT_EQ(participant->GetMediaSession(), session);
    EXPECT_EQ(participant->GetInfo().media_session_id, 8001U);

    auto replacement = std::make_shared<room::ParticipantSession>();
    replacement->id = 8002;
    replacement->endpoint = std::make_shared<media::SfuEndpoint>(8002);
    ASSERT_TRUE(replacement->endpoint->Start());
    EXPECT_FALSE(participant->BindMediaSession(replacement));
    EXPECT_FALSE(participant->BindEndpoint(replacement->endpoint));
    EXPECT_EQ(participant->GetEndpoint(), endpoint);
    EXPECT_EQ(participant->GetMediaSession(), session);
    replacement->endpoint->Stop();
    participant->Leave();
}

TEST(Participant, LeaveDetachesBindingsBeforeCallbackAndCannotRebind)
{
    auto participant = std::make_shared<room::Participant>("p-1", "Alice");
    auto endpoint = std::make_shared<media::SfuEndpoint>(8003);
    ASSERT_TRUE(endpoint->Start());
    auto session = std::make_shared<room::ParticipantSession>();
    session->id = 8003;
    session->endpoint = endpoint;
    ASSERT_TRUE(participant->BindMediaSession(session));
    ASSERT_TRUE(participant->BindSignaling("ws-1"));
    media::TrackInfo track;
    track.sid = "camera";
    ASSERT_TRUE(participant->AddPublishedTrack(std::make_shared<media::MediaTrack>(track)));
    ASSERT_TRUE(participant->SubscribeTrack("remote-camera"));
    std::weak_ptr<room::ParticipantSession> weak_session = session;
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

    auto replacement = std::make_shared<room::ParticipantSession>();
    replacement->id = 8004;
    replacement->endpoint = std::make_shared<media::SfuEndpoint>(8004);
    ASSERT_TRUE(replacement->endpoint->Start());
    EXPECT_FALSE(participant->BindMediaSession(replacement));
    replacement->endpoint->Stop();
}
