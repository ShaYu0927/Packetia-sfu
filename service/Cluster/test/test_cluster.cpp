#include "Coordinator.h"
#include "ClusterStore.h"
#include "MeetingService.h"
#include "RoomAdminService.h"
#include "NodeLease.h"
#include "JoinToken.h"
#include "JwtAuthenticator.h"
#include "KeyProvider.h"
#include "RoomAccessPolicy.h"
#include <gtest/gtest.h>
#include <filesystem>
#include <future>
#include <set>

namespace
{
using namespace service::cluster;
class ClusterTest : public ::testing::Test
{
protected:
    int64_t now = Coordinator::UnixSeconds();
    std::string path = (std::filesystem::temp_directory_path() / ("packetia-cluster-" + Coordinator::RandomId() + ".db")).string();
    std::shared_ptr<ClusterStore> store;
    std::unique_ptr<Coordinator> coordinator;
    std::unique_ptr<service::meeting::MeetingService> meetings;
    Node node{"sfu-a", "boot-a", "ws://127.0.0.1:12345", 100, 0};
    void SetUp() override { Open(); }
    void Open() { store = std::make_shared<ClusterStore>(path); coordinator = std::make_unique<Coordinator>(store, [this] { return now; }); meetings = std::make_unique<service::meeting::MeetingService>(store); }
    struct Admission : RoomAssignment { bool can_publish, room_admin; };
    Admission Admit(const std::string& room, const std::string& user)
    { const auto grant = meetings->Grant(room, user); return {coordinator->AssignRoom(room), grant.can_publish, grant.room_admin}; }
    void TearDown() override
    {
        meetings.reset(); coordinator.reset(); store.reset();
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
    const auto room = meetings->CreateRoom("alice");
    Coordinator second_writer(path, [this] { return now; });
    std::vector<std::future<RoomAssignment>> calls;
    for (int i = 0; i < 24; ++i)
        calls.push_back(std::async(std::launch::async, [&, i] {
            return (i % 2 ? second_writer : *coordinator).AssignRoom(room);
        }));
    const auto first = calls.front().get();
    for (std::size_t i = 1; i < calls.size(); ++i) EXPECT_EQ(calls[i].get().node.id, first.node.id);
    coordinator.reset(); Open();
    EXPECT_EQ(Admit(room, "alice").node.instance, first.node.instance);
}
TEST_F(ClusterTest, CapacitySpreadsRoomsAndPinsFullNodes)
{
    node.max_rooms = 1;
    coordinator->Register(node);
    coordinator->Register({"sfu-b", "boot-b", "ws://localhost:12346", 1, 0});
    const auto one = meetings->CreateRoom("alice"), two = meetings->CreateRoom("alice"), three = meetings->CreateRoom("alice");
    const auto first = Admit(one, "alice");
    EXPECT_NE(first.node.id, Admit(two, "alice").node.id);
    EXPECT_EQ(first.node.id, Admit(one, "alice").node.id);
    FailureCode([&] { Admit(three, "alice"); }, "no_available_node");
}
TEST_F(ClusterTest, ExpiryDoesNotRemapRoomAndSameInstanceCanRecover)
{
    coordinator->Register(node);
    const auto room = meetings->CreateRoom("alice");
    Admit(room, "alice");
    now += Coordinator::kLeaseSeconds;
    coordinator->Register({"sfu-b", "boot-b", "ws://localhost:12346", 100, 0});
    FailureCode([&] { Admit(room, "alice"); }, "room_node_unavailable");
    coordinator->Heartbeat(node.id, node.instance, 1);
    EXPECT_EQ(Admit(room, "alice").node.id, node.id);
}
TEST_F(ClusterTest, InstanceFencingSurvivesControlRestart)
{
    coordinator->Register(node);
    const auto room = meetings->CreateRoom("alice");
    Admit(room, "alice");
    auto replacement = node; replacement.instance = "boot-new";
    FailureCode([&] { coordinator->Register(replacement); }, "node_already_live");
    coordinator.reset(); Open();
    FailureCode([&] { coordinator->Register(replacement); }, "node_already_live");
    now += Coordinator::kLeaseSeconds;
    coordinator->Register(replacement);
    FailureCode([&] { coordinator->Heartbeat(node.id, node.instance, 0); }, "instance_fenced");
    FailureCode([&] { Admit(room, "alice"); }, "room_node_unavailable");
    now += Coordinator::kLeaseSeconds;
    coordinator.reset(); Open();
    FailureCode([&] { coordinator->Register(node); }, "instance_retired");
    const auto new_room = meetings->CreateRoom("alice");
    coordinator->Register(replacement);
    EXPECT_EQ(Admit(new_room, "alice").node.instance, "boot-new");
}
TEST_F(ClusterTest, MembershipAndOwnerPolicyArePersistent)
{
    coordinator->Register(node);
    const auto room = meetings->CreateRoom("alice");
    FailureCode([&] { Admit(room, "mallory"); }, "room_membership_required");
    FailureCode([&] { meetings->SetMemberRole(room, "mallory", "mallory", true); }, "room_owner_required");
    meetings->SetMemberRole(room, "alice", "bob", false);
    coordinator.reset(); Open();
    EXPECT_FALSE(Admit(room, "bob").can_publish);
    EXPECT_FALSE(Admit(room, "bob").room_admin);
    EXPECT_TRUE(Admit(room, "alice").can_publish);
    EXPECT_TRUE(Admit(room, "alice").room_admin);
    meetings->SetMemberRole(room, "alice", "bob", true);
    EXPECT_TRUE(Admit(room, "bob").can_publish);
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
TEST_F(ClusterTest, ReleaseIsFencedAndRecoversCapacity)
{
    node.max_rooms = 1;
    coordinator->Register(node);
    const auto room = meetings->CreateRoom("alice");
    const auto other = meetings->CreateRoom("alice");
    const auto first = coordinator->AssignRoom(room);
    FailureCode([&] { coordinator->AssignRoom(other); }, "no_available_node");
    EXPECT_TRUE(coordinator->ReclaimCandidates(node.id, node.instance).empty());
    now += Coordinator::kAdmissionHoldSeconds;
    coordinator->Heartbeat(node.id, node.instance, 0);
    const auto candidates = coordinator->ReclaimCandidates(node.id, node.instance);
    ASSERT_EQ(candidates.size(), 1u);
    EXPECT_EQ(candidates.front().assignment_id, first.assignment_id);
    EXPECT_FALSE(coordinator->ReleaseRoom(node.id, node.instance, room, "wrong-epoch"));
    EXPECT_TRUE(coordinator->ReleaseRoom(node.id, node.instance, room, first.assignment_id));
    EXPECT_FALSE(coordinator->FindRoom(room).has_value());
    const auto next = coordinator->AssignRoom(room);
    EXPECT_NE(next.assignment_id, first.assignment_id);
    EXPECT_FALSE(coordinator->ReleaseRoom(node.id, node.instance, room, first.assignment_id));
    EXPECT_EQ(coordinator->FindRoom(room)->assignment_id, next.assignment_id);
    EXPECT_TRUE(coordinator->ReleaseRoom(node.id, node.instance, room, next.assignment_id));
    EXPECT_NO_THROW(coordinator->AssignRoom(other));
}
TEST_F(ClusterTest, EndPersistsClosingUntilOwnerConfirmsAndRecoversQuota)
{
    coordinator->Register(node);
    const auto room = meetings->CreateRoom("alice");
    const auto assignment = coordinator->AssignRoom(room);
    FailureCode([&] { meetings->BeginEnd(room, "mallory"); }, "room_owner_required");
    EXPECT_FALSE(meetings->BeginEnd(room, "alice"));
    FailureCode([&] { meetings->Grant(room, "alice"); }, "room_ended");
    FailureCode([&] { coordinator->AssignRoom(room); }, "room_ended");
    coordinator.reset(); Open();
    EXPECT_TRUE(coordinator->FindRoom(room, true)->closing);
    ASSERT_EQ(coordinator->ReclaimCandidates(node.id, node.instance).size(), 1u);
    EXPECT_TRUE(coordinator->ReleaseRoom(node.id, node.instance, room, assignment.assignment_id));
    EXPECT_TRUE(meetings->BeginEnd(room, "alice"));
    for (int i = 0; i < 100; ++i) meetings->CreateRoom("alice");
    FailureCode([&] { meetings->CreateRoom("alice"); }, "room_quota_exceeded");
}
TEST_F(ClusterTest, ReclaimCandidatesRotateAndNewAdmissionsExtendReservation)
{
    coordinator->Register(node);
    const auto room = meetings->CreateRoom("alice");
    const auto first = coordinator->AssignRoom(room);
    now += 60;
    coordinator->Heartbeat(node.id, node.instance, 0);
    EXPECT_EQ(coordinator->AssignRoom(room).assignment_id, first.assignment_id);
    now += 31;
    coordinator->Heartbeat(node.id, node.instance, 0);
    EXPECT_TRUE(coordinator->ReclaimCandidates(node.id, node.instance).empty());
    for (int i = 0; i < 19; ++i) coordinator->AssignRoom(meetings->CreateRoom("alice"));
    now += Coordinator::kAdmissionHoldSeconds;
    coordinator->Heartbeat(node.id, node.instance, 0);
    const auto batch = coordinator->ReclaimCandidates(node.id, node.instance);
    ASSERT_EQ(batch.size(), 16u);
    ++now;
    const auto next = coordinator->ReclaimCandidates(node.id, node.instance);
    std::set<std::string> seen;
    for (const auto& item : batch) seen.insert(item.room_id);
    for (const auto& item : next) seen.insert(item.room_id);
    EXPECT_EQ(seen.size(), 20u);
}
TEST_F(ClusterTest, LegacyDatabaseMigratesBindingsWithoutLosingMembers)
{
    meetings.reset(); coordinator.reset(); store.reset();
    std::filesystem::remove(path);
    sqlite3* db = nullptr;
    ASSERT_EQ(sqlite3_open(path.c_str(), &db), SQLITE_OK);
    ASSERT_EQ(sqlite3_exec(db,
        "CREATE TABLE cluster_nodes(id TEXT PRIMARY KEY,instance TEXT NOT NULL,url TEXT NOT NULL,capacity INTEGER NOT NULL,expires INTEGER NOT NULL,participants INTEGER NOT NULL);"
        "CREATE TABLE cluster_rooms(id TEXT PRIMARY KEY,owner TEXT NOT NULL,node TEXT,instance TEXT);"
        "CREATE TABLE cluster_members(room TEXT NOT NULL,user TEXT NOT NULL,publish INTEGER NOT NULL,PRIMARY KEY(room,user));"
        "INSERT INTO cluster_rooms VALUES('legacy','alice','sfu-a','boot-a');"
        "INSERT INTO cluster_members VALUES('legacy','alice',1);", nullptr, nullptr, nullptr), SQLITE_OK);
    sqlite3_close(db);
    Open(); coordinator->Register(node);
    EXPECT_TRUE(meetings->Grant("legacy", "alice").room_admin);
    const auto assignment = coordinator->AssignRoom("legacy");
    EXPECT_FALSE(assignment.assignment_id.empty());
    EXPECT_EQ(assignment.node.id, node.id);
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
    context.video.room_admin = true;
    context.video.can_update_own_metadata = true;
    context.node_id = "sfu-a"; context.instance_id = "boot-a"; context.assignment_id = "epoch-a";
    const auto token = auth::SignJoinToken(context, secret, now);
    auth::JwtAuthenticator verifier(std::make_shared<auth::StaticKeyProvider>(std::map<std::string,std::string>{{"test",secret}}), [=] { return now; });
    auto valid = verifier.Authenticate(token, auth::Audience::WebRtc);
    ASSERT_TRUE(valid.Succeeded());
    EXPECT_TRUE(valid.context->video.room_admin);
    EXPECT_TRUE(valid.context->video.can_update_own_metadata);
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
