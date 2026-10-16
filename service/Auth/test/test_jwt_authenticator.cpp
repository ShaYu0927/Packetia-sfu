#include "AuthService.h"
#include "JwtAuthenticator.h"
#include "RoomAccessPolicy.h"
#include "third/nlohmann/json.hpp"
#include <gtest/gtest.h>
#include <openssl/evp.h>
#include <openssl/hmac.h>
#include <atomic>
#include <limits>
#include <thread>
#include <vector>

namespace service::auth
{
namespace
{
using Json = nlohmann::json;
constexpr char kSecret[] = "0123456789abcdef0123456789abcdef";
// Independent fixture generated with Python's json/base64/hmac/hashlib libraries.
constexpr char kKnownToken[] =
    "eyJhbGciOiJIUzI1NiIsInR5cCI6IkpXVCJ9."
    "eyJpc3MiOiJhcGktYSIsInN1YiI6ImFsaWNlIiwiYXVkIjoicGFja2V0aWEtd2VicnRjIiwiZXhwIjoyMDAsImlhdCI6OTAsIm5iZiI6OTAsIm5hbWUiOiJBbGljZSIsInZpZGVvIjp7InJvb20iOiJyb29tLWEiLCJyb29tSm9pbiI6dHJ1ZSwiY2FuUHVibGlzaCI6dHJ1ZSwiY2FuU3Vic2NyaWJlIjpmYWxzZX19."
    "eq5nY5Kw3aJqaVQrByQDPqYoUOuxkm6mgOwvMQ4KzSE";

std::string Encode(std::string_view bytes)
{
    std::string result(4 * ((bytes.size() + 2) / 3) + 1, '\0');
    const auto size = EVP_EncodeBlock(reinterpret_cast<unsigned char*>(result.data()),
        reinterpret_cast<const unsigned char*>(bytes.data()), static_cast<int>(bytes.size()));
    result.resize(size);
    for (auto& c : result)
    {
        if (c == '+') c = '-';
        else if (c == '/') c = '_';
    }
    while (!result.empty() && result.back() == '=') result.pop_back();
    return result;
}

std::string SignRaw(const std::string& claims, const std::string& header = R"({"alg":"HS256","typ":"JWT"})",
                    const std::string& secret = kSecret)
{
    const auto message = Encode(header) + "." + Encode(claims);
    unsigned char digest[EVP_MAX_MD_SIZE];
    unsigned length = 0;
    HMAC(EVP_sha256(), secret.data(), static_cast<int>(secret.size()),
         reinterpret_cast<const unsigned char*>(message.data()), message.size(), digest, &length);
    return message + "." + Encode(std::string_view(reinterpret_cast<const char*>(digest), length));
}

Json Claims()
{
    return {{"iss", "api-a"}, {"sub", "alice"}, {"aud", "packetia-webrtc"}, {"exp", 200},
            {"iat", 90}, {"nbf", 90}, {"video", {{"room", "room-a"}, {"roomJoin", true},
             {"canPublish", true}, {"canSubscribe", false}}}};
}

std::shared_ptr<const IKeyProvider> Keys()
{
    return std::make_shared<StaticKeyProvider>(std::map<std::string, std::string>{{"api-a", kSecret}});
}

class JwtTest : public ::testing::Test
{
protected:
    JwtAuthenticator verifier{Keys(), [] { return 100; }};
    AuthResult Verify(const Json& claims) { return verifier.Authenticate(SignRaw(claims.dump()), Audience::WebRtc); }
};

TEST_F(JwtTest, IndependentFixtureVerifiesAndBindsClaims)
{
    const auto result = verifier.Authenticate(kKnownToken, Audience::WebRtc);
    ASSERT_TRUE(result.Succeeded());
    EXPECT_EQ(result.context->issuer, "api-a");
    EXPECT_EQ(result.context->subject_id, "alice");
    EXPECT_EQ(result.context->name, "Alice");
    EXPECT_EQ(result.context->expires_at_unix_seconds, 200);
    EXPECT_EQ(result.context->video.room, "room-a");
    EXPECT_TRUE(result.context->video.room_join);
    EXPECT_TRUE(result.context->video.can_publish);
    EXPECT_FALSE(result.context->video.can_subscribe);
}

TEST_F(JwtTest, KeyLookupAndSignatureMustBothMatch)
{
    auto claims = Claims();
    claims["iss"] = "unknown";
    EXPECT_EQ(Verify(claims).error, AuthError::InvalidToken);
    EXPECT_EQ(verifier.Authenticate(SignRaw(Claims().dump(), R"({"alg":"HS256"})", "wrong-secret"),
                                   Audience::WebRtc).error, AuthError::InvalidToken);
    std::string tampered = kKnownToken;
    const auto second = tampered.rfind('.');
    tampered[second + 1] = tampered[second + 1] == 'a' ? 'b' : 'a';
    EXPECT_EQ(verifier.Authenticate(tampered, Audience::WebRtc).error, AuthError::InvalidToken);
    JwtAuthenticator absent(nullptr);
    EXPECT_EQ(absent.Authenticate(kKnownToken, Audience::WebRtc).error, AuthError::NotConfigured);
}

TEST_F(JwtTest, RejectsAlgorithmConfusionAndUnsupportedJoseHeaders)
{
    for (const auto& header : {R"({"alg":"none"})", R"({"alg":"RS256"})", R"({"alg":"HS512"})",
         R"({"alg":"HS256","crit":["anything"]})", R"({"alg":"HS256","b64":false})",
         R"({"alg":"HS256","typ":"JWE"})", R"({"alg":256})"})
    {
        SCOPED_TRACE(header);
        EXPECT_EQ(verifier.Authenticate(SignRaw(Claims().dump(), header), Audience::WebRtc).error,
                  AuthError::InvalidToken);
    }
}

TEST_F(JwtTest, RejectsWeakKeyConfigurationsAndUnsupportedSourceRestrictions)
{
    const std::string weak_secret = "short-secret";
    auto weak_keys = std::make_shared<StaticKeyProvider>(std::map<std::string, std::string>{{"api-a", weak_secret}});
    JwtAuthenticator weak_verifier(weak_keys, [] { return 100; });
    EXPECT_EQ(weak_verifier.Authenticate(SignRaw(Claims().dump(), R"({"alg":"HS256"})", weak_secret),
                                        Audience::WebRtc).error, AuthError::InvalidToken);
    auto claims = Claims();
    claims["video"]["canPublishSources"] = Json::array({"camera"});
    EXPECT_EQ(Verify(claims).error, AuthError::InvalidToken);
}

TEST_F(JwtTest, AudienceIsRequiredAndSupportsStandardArrays)
{
    auto claims = Claims();
    claims["aud"] = "packetia-control";
    EXPECT_EQ(Verify(claims).error, AuthError::WrongAudience);
    EXPECT_TRUE(verifier.Authenticate(SignRaw(claims.dump()), Audience::Control).Succeeded());
    claims.erase("aud");
    EXPECT_EQ(Verify(claims).error, AuthError::WrongAudience);
    claims["aud"] = Json::array({"other", "packetia-webrtc"});
    EXPECT_TRUE(Verify(claims).Succeeded());
    claims["aud"] = Json::array({"packetia-webrtc", 7});
    EXPECT_EQ(Verify(claims).error, AuthError::WrongAudience);
}

TEST_F(JwtTest, EnforcesExpirationAndOptionalAdmissionTimeClaims)
{
    auto claims = Claims();
    claims["exp"] = 100;
    EXPECT_EQ(Verify(claims).error, AuthError::ExpiredToken);
    claims = Claims();
    claims["nbf"] = 101;
    EXPECT_EQ(Verify(claims).error, AuthError::InvalidToken);
    claims = Claims();
    claims["iat"] = 101;
    EXPECT_EQ(Verify(claims).error, AuthError::InvalidToken);
    claims = Claims();
    claims.erase("iat");
    claims.erase("nbf");
    EXPECT_TRUE(Verify(claims).Succeeded());
    claims.erase("exp");
    EXPECT_EQ(Verify(claims).error, AuthError::InvalidToken);
}

TEST_F(JwtTest, TimestampsMustBeNonnegativeIntegersWithoutOverflow)
{
    const std::vector<Json> invalid{Json("200"), Json(200.5), Json(-1), Json(true), Json(nullptr),
                                  Json(std::numeric_limits<uint64_t>::max())};
    for (const char* name : {"exp", "nbf", "iat"})
    {
        for (const auto& value : invalid)
        {
            SCOPED_TRACE(std::string(name) + ":" + value.dump());
            auto claims = Claims();
            claims[name] = value;
            EXPECT_EQ(Verify(claims).error, AuthError::InvalidToken);
        }
    }
}

TEST_F(JwtTest, MissingCapabilitiesDenyInsteadOfGrantingDefaults)
{
    auto claims = Claims();
    claims["video"] = {{"room", "room-a"}, {"roomJoin", true}};
    const auto result = Verify(claims);
    ASSERT_TRUE(result.Succeeded());
    EXPECT_FALSE(result.context->video.can_publish);
    EXPECT_FALSE(result.context->video.can_subscribe);
    RoomAccessPolicy policy;
    ResourceContext room;
    room.id = "room-a";
    EXPECT_TRUE(policy.Check(*result.context, Action::JoinRoom, room).Allowed());
    EXPECT_FALSE(policy.Check(*result.context, Action::Publish, room).Allowed());
    claims.erase("video");
    const auto no_grants = Verify(claims);
    ASSERT_TRUE(no_grants.Succeeded());
    EXPECT_FALSE(policy.Check(*no_grants.context, Action::JoinRoom, room).Allowed());
}

TEST_F(JwtTest, RejectsMalformedClaimTypesAndIdentity)
{
    for (const auto* field : {"iss", "sub", "name", "jti"})
    {
        auto claims = Claims();
        claims[field] = 123;
        EXPECT_EQ(Verify(claims).error, AuthError::InvalidToken);
    }
    for (const auto* field : {"roomJoin", "canPublish", "canSubscribe"})
    {
        auto claims = Claims();
        claims["video"][field] = "true";
        EXPECT_EQ(Verify(claims).error, AuthError::InvalidToken);
    }
    auto claims = Claims();
    claims["sub"] = "";
    EXPECT_EQ(Verify(claims).error, AuthError::InvalidToken);
    claims["sub"] = std::string("alice\0bob", 9);
    EXPECT_EQ(Verify(claims).error, AuthError::InvalidToken);
    claims = Claims();
    claims["video"] = Json::array();
    EXPECT_EQ(Verify(claims).error, AuthError::InvalidToken);
}

TEST_F(JwtTest, RejectsDuplicateClaimsAtEveryObjectLevel)
{
    auto body = Claims().dump();
    body.insert(1, R"("sub":"mallory",)");
    EXPECT_EQ(verifier.Authenticate(SignRaw(body), Audience::WebRtc).error, AuthError::InvalidToken);
    body = Claims().dump();
    body.insert(body.find("\"roomJoin\""), R"("roomJoin":false,)");
    EXPECT_EQ(verifier.Authenticate(SignRaw(body), Audience::WebRtc).error, AuthError::InvalidToken);
    EXPECT_EQ(verifier.Authenticate(SignRaw(Claims().dump(), R"({"alg":"none","alg":"HS256"})"),
                                   Audience::WebRtc).error, AuthError::InvalidToken);
}

TEST_F(JwtTest, RejectsMalformedAndNoncanonicalCompactSerialization)
{
    for (const auto& token : {std::string("a.b.c"), std::string(kKnownToken) + ".", std::string(kKnownToken) + "=",
                             std::string(" ") + kKnownToken, std::string(8193, 'a')})
        EXPECT_EQ(verifier.Authenticate(token, Audience::WebRtc).error, AuthError::InvalidToken);
    std::string alias = kKnownToken;
    alias.back() = 'F'; // E -> F changes only unused base64url trailing bits.
    EXPECT_EQ(verifier.Authenticate(alias, Audience::WebRtc).error, AuthError::InvalidToken);
    EXPECT_EQ(verifier.Authenticate("", Audience::WebRtc).error, AuthError::Unauthenticated);
    EXPECT_EQ(verifier.Authenticate(SignRaw("[]"), Audience::WebRtc).error, AuthError::InvalidToken);
    EXPECT_EQ(verifier.Authenticate(SignRaw("{"), Audience::WebRtc).error, AuthError::InvalidToken);
}

TEST_F(JwtTest, LimitsJsonNestingEvenForIgnoredClaims)
{
    auto claims = Claims();
    Json nested = true;
    for (unsigned i = 0; i < 20; ++i) nested = Json::array({nested});
    claims["extra"] = std::move(nested);
    EXPECT_EQ(Verify(claims).error, AuthError::InvalidToken);
}

TEST_F(JwtTest, ConcurrentVerificationDoesNotShareMutableSessionState)
{
    std::atomic<unsigned> failures{0};
    std::vector<std::thread> workers;
    for (unsigned i = 0; i < 4; ++i)
        workers.emplace_back([&] {
            for (unsigned count = 0; count < 100; ++count)
                if (!verifier.Authenticate(kKnownToken, Audience::WebRtc).Succeeded()) ++failures;
        });
    for (auto& worker : workers) worker.join();
    EXPECT_EQ(failures, 0u);
}

TEST(RoomAccessPolicy, ScopesActionsToExactlyOneRoomAndAudience)
{
    auto caller = JwtAuthenticator(Keys(), [] { return 100; }).Authenticate(kKnownToken, Audience::WebRtc).context.value();
    RoomAccessPolicy policy;
    ResourceContext room;
    room.id = "room-a";
    EXPECT_TRUE(policy.Check(caller, Action::JoinRoom, room).Allowed());
    EXPECT_TRUE(policy.Check(caller, Action::ListTracks, room).Allowed());
    EXPECT_TRUE(policy.Check(caller, Action::Publish, room).Allowed());
    EXPECT_FALSE(policy.Check(caller, Action::Subscribe, room).Allowed());
    EXPECT_FALSE(policy.Check(caller, Action::ManageService, room).Allowed());
    EXPECT_FALSE(policy.Check(caller, Action::ControlConference, room).Allowed());
    room.id = "room-b";
    EXPECT_FALSE(policy.Check(caller, Action::Publish, room).Allowed());
    room.id = "room-a";
    room.room_id = "room-b";
    EXPECT_FALSE(policy.Check(caller, Action::Publish, room).Allowed());
    room.room_id.clear();
    caller.audience = Audience::Control;
    EXPECT_FALSE(policy.Check(caller, Action::JoinRoom, room).Allowed());
}

TEST(RoomAccessPolicy, TrackPermissionsUseTrustedContainingRoom)
{
    AuthContext caller;
    caller.subject_id = "viewer";
    caller.video = {"room-a", true, false, true};
    ResourceContext track;
    track.type = ResourceType::Track;
    track.id = "track-1";
    track.room_id = "room-a";
    RoomAccessPolicy policy;
    EXPECT_TRUE(policy.Check(caller, Action::Subscribe, track).Allowed());
    track.room_id = "room-b";
    EXPECT_FALSE(policy.Check(caller, Action::Subscribe, track).Allowed());
    track.room_id = "room-a";
    caller.video.room_join = false;
    EXPECT_FALSE(policy.Check(caller, Action::Subscribe, track).Allowed());
}
}
}
