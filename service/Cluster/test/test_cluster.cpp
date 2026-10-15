#include "service/Cluster/Coordinator.h"
#include "service/Cluster/NodeLease.h"
#include "service/Auth/JoinToken.h"
#include "service/Auth/JwtAuthenticator.h"
#include "service/Auth/KeyProvider.h"
#include "service/Auth/RoomAccessPolicy.h"
#include <gtest/gtest.h>
#include <filesystem>
#include <future>

namespace
{
using namespace service::cluster;
class ClusterTest : public ::testing::Test
{
protected:
    int64_t now = Coordinator::UnixSeconds();
    std::string path = (std::filesystem::temp_directory_path() / ("packetia-cluster-" + Coordinator::RandomId() + ".db")).string();
    std::unique_ptr<Coordinator> coordinator;
    Node node{"sfu-a", "boot-a", "ws://127.0.0.1:12345", 100, 0};
    void SetUp() override { Open(); }
    void Open() { coordinator = std::make_unique<Coordinator>(path, [this] { return now; }); }
    void TearDown() override
    {
        coordinator.reset();
        for (const auto& suffix : {"", "-wal", "-shm"}) std::filesystem::remove(path + suffix);
    }
    void FailureCode(const std::function<void()>& call, const char* expected)
    {
        try { call(); FAIL() << "expected " << expected; }
        catch (const Failure& error) { EXPECT_STREQ(error.code, expected); }
    }
};
TEST_F(ClusterTest, ConcurrentFirstAdmissionsHaveOneDurableAssignment)
{
    coordinator->Register(node);
    coordinator->Register({"sfu-b", "boot-b", "ws://localhost:12346", 100, 0});
    const auto room = coordinator->CreateRoom("alice");
    Coordinator second_writer(path, [this] { return now; });
    std::vector<std::future<Admission>> calls;
    for (int i = 0; i < 24; ++i)
        calls.push_back(std::async(std::launch::async, [&, i] {
            return (i % 2 ? second_writer : *coordinator).Admit(room, "alice");
        }));
    const auto first = calls.front().get();
    for (std::size_t i = 1; i < calls.size(); ++i) EXPECT_EQ(calls[i].get().node.id, first.node.id);
    coordinator.reset(); Open();
    EXPECT_EQ(coordinator->Admit(room, "alice").node.instance, first.node.instance);
}
TEST_F(ClusterTest, CapacitySpreadsRoomsAndPinsFullNodes)
{
    node.max_rooms = 1;
    coordinator->Register(node);
    coordinator->Register({"sfu-b", "boot-b", "ws://localhost:12346", 1, 0});
    const auto one = coordinator->CreateRoom("alice"), two = coordinator->CreateRoom("alice"), three = coordinator->CreateRoom("alice");
    const auto first = coordinator->Admit(one, "alice");
    EXPECT_NE(first.node.id, coordinator->Admit(two, "alice").node.id);
    EXPECT_EQ(first.node.id, coordinator->Admit(one, "alice").node.id);
    FailureCode([&] { coordinator->Admit(three, "alice"); }, "no_available_node");
}
TEST_F(ClusterTest, ExpiryDoesNotRemapRoomAndSameInstanceCanRecover)
{
    coordinator->Register(node);
    const auto room = coordinator->CreateRoom("alice");
    coordinator->Admit(room, "alice");
    now += Coordinator::kLeaseSeconds;
    coordinator->Register({"sfu-b", "boot-b", "ws://localhost:12346", 100, 0});
    FailureCode([&] { coordinator->Admit(room, "alice"); }, "room_node_unavailable");
    coordinator->Heartbeat(node.id, node.instance, 1);
    EXPECT_EQ(coordinator->Admit(room, "alice").node.id, node.id);
}
TEST_F(ClusterTest, InstanceFencingSurvivesControlRestart)
{
    coordinator->Register(node);
    const auto room = coordinator->CreateRoom("alice");
    coordinator->Admit(room, "alice");
    auto replacement = node; replacement.instance = "boot-new";
    FailureCode([&] { coordinator->Register(replacement); }, "node_already_live");
    coordinator.reset(); Open();
    FailureCode([&] { coordinator->Register(replacement); }, "node_already_live");
    now += Coordinator::kLeaseSeconds;
    coordinator->Register(replacement);
    FailureCode([&] { coordinator->Heartbeat(node.id, node.instance, 0); }, "instance_fenced");
    FailureCode([&] { coordinator->Admit(room, "alice"); }, "room_node_unavailable");
    now += Coordinator::kLeaseSeconds;
    coordinator.reset(); Open();
    FailureCode([&] { coordinator->Register(node); }, "instance_retired");
    const auto new_room = coordinator->CreateRoom("alice");
    coordinator->Register(replacement);
    EXPECT_EQ(coordinator->Admit(new_room, "alice").node.instance, "boot-new");
}
TEST_F(ClusterTest, MembershipAndOwnerPolicyArePersistent)
{
    coordinator->Register(node);
    const auto room = coordinator->CreateRoom("alice");
    FailureCode([&] { coordinator->Admit(room, "mallory"); }, "room_membership_required");
    FailureCode([&] { coordinator->AddMember(room, "mallory", "mallory", true); }, "room_owner_required");
    coordinator->AddMember(room, "alice", "bob", false);
    coordinator.reset(); Open();
    EXPECT_FALSE(coordinator->Admit(room, "bob").can_publish);
    EXPECT_TRUE(coordinator->Admit(room, "alice").can_publish);
    coordinator->AddMember(room, "alice", "bob", true);
    EXPECT_TRUE(coordinator->Admit(room, "bob").can_publish);
}
TEST_F(ClusterTest, RegistrationIsIdempotentAndConfigurationIsImmutable)
{
    coordinator->Register(node);
    coordinator->Register(node);
    auto changed = node; changed.signaling_url = "ws://elsewhere:9999";
    FailureCode([&] { coordinator->Register(changed); }, "instance_configuration_changed");
    changed = node; changed.max_rooms = 0;
    FailureCode([&] { coordinator->Register(changed); }, "invalid_node");
}
TEST(ClusterAuth, SignedJoinTokenEnforcesAudienceRoomPermissionsAndNode)
{
    namespace auth = service::auth;
    const std::string secret(40, 'x');
    const int64_t now = Coordinator::UnixSeconds();
    auth::AuthContext context;
    context.subject_id = "alice"; context.issuer = "test";
    context.expires_at_unix_seconds = now + 60;
    context.video = {"room-a", true, false, true};
    context.node_id = "sfu-a"; context.instance_id = "boot-a";
    const auto token = auth::SignJoinToken(context, secret, now);
    auth::JwtAuthenticator verifier(std::make_shared<auth::StaticKeyProvider>(std::map<std::string,std::string>{{"test",secret}}), [=] { return now; });
    auto valid = verifier.Authenticate(token, auth::Audience::WebRtc);
    ASSERT_TRUE(valid.Succeeded());
    EXPECT_FALSE(verifier.Authenticate("", auth::Audience::WebRtc).Succeeded());
    EXPECT_FALSE(verifier.Authenticate(token + "x", auth::Audience::WebRtc).Succeeded());
    EXPECT_FALSE(verifier.Authenticate(token, auth::Audience::Control).Succeeded());
    auth::JwtAuthenticator wrong_key(std::make_shared<auth::StaticKeyProvider>(std::map<std::string,std::string>{{"test",std::string(40,'y')}}));
    EXPECT_FALSE(wrong_key.Authenticate(token, auth::Audience::WebRtc).Succeeded());
    auth::JwtAuthenticator expired(std::make_shared<auth::StaticKeyProvider>(std::map<std::string,std::string>{{"test",secret}}), [=] { return now + 60; });
    EXPECT_FALSE(expired.Authenticate(token, auth::Audience::WebRtc).Succeeded());
    NodeLease lease("sfu-a", "boot-a");
    EXPECT_FALSE(lease.Allows(*valid.context));
    lease.Renew(NodeLease::NowMs(), 10);
    EXPECT_TRUE(lease.Allows(*valid.context));
    NodeLease wrong_node("sfu-b", "boot-a"), wrong_instance("sfu-a", "boot-b");
    wrong_node.Renew(NodeLease::NowMs(), 10); wrong_instance.Renew(NodeLease::NowMs(), 10);
    EXPECT_FALSE(wrong_node.Allows(*valid.context));
    EXPECT_FALSE(wrong_instance.Allows(*valid.context));
    auto unbound = *valid.context; unbound.node_id.clear();
    EXPECT_FALSE(lease.Allows(unbound));
    auth::RoomAccessPolicy policy;
    auth::ResourceContext room; room.type = auth::ResourceType::Room; room.id = room.room_id = "room-a";
    EXPECT_TRUE(policy.Check(*valid.context, auth::Action::JoinRoom, room).Allowed());
    EXPECT_FALSE(policy.Check(*valid.context, auth::Action::Publish, room).Allowed());
    room.id = room.room_id = "room-b";
    EXPECT_FALSE(policy.Check(*valid.context, auth::Action::JoinRoom, room).Allowed());
    context.video.room_join = false;
    EXPECT_FALSE(policy.Check(context, auth::Action::JoinRoom, room).Allowed());
    lease.Renew(NodeLease::NowMs() - 10000, 10);
    EXPECT_FALSE(lease.Allows(*valid.context));
    lease.Renew(NodeLease::NowMs(), 10); lease.Revoke();
    EXPECT_FALSE(lease.Allows(*valid.context));
}
}
