#include "AuthService.h"
#include "RtspDigestAuthenticator.h"
#include "StreamAccessPolicy.h"
#include <gtest/gtest.h>
#include <openssl/evp.h>

namespace service::auth
{
namespace
{
std::string Md5(const std::string& value)
{
    unsigned char bytes[16]; unsigned int size = 0;
    if (EVP_Digest(value.data(), value.size(), bytes, &size, EVP_md5(), nullptr) != 1 || size != 16) return {};
    const char* hex = "0123456789abcdef";
    std::string result;
    for (auto byte : bytes) { result += hex[byte >> 4]; result += hex[byte & 15]; }
    return result;
}
std::string Nonce(const std::string& challenge)
{
    auto pos = challenge.find("nonce=\"");
    if (pos == std::string::npos) return {};
    pos += 7;
    return challenge.substr(pos, challenge.find('"', pos) - pos);
}
std::string Header(const std::string& nonce, const std::string& method = "ANNOUNCE",
    const std::string& uri = "rtsp://host/live/camera", const std::string& nc = "00000001", bool qop = true,
    const std::string& password = "secret")
{
    const auto ha1 = Md5("alice:packetia:" + password);
    auto value = ha1 + ":" + nonce + ":";
    if (qop) value += nc + ":client:auth:";
    const auto response = Md5(value + Md5(method + ":" + uri));
    return "Digest username=\"alice\", realm=\"packetia\", nonce=\"" + nonce + "\", uri=\"" + uri +
        "\", response=\"" + response + "\"" + (qop ? ", qop=auth, nc=" + nc + ", cnonce=\"client\"" : "");
}
class RtspDigestTest : public testing::Test
{
protected:
    void SetUp() override
    {
        RtspAuthOptions options;
        options.enabled = true;
        options.users = {{"alice", Md5("alice:packetia:secret"), {{"live/camera", true, false}, {"live/view", false, true}}}};
        provider = std::make_shared<StaticRtspCredentialProvider>(options);
        digest = std::make_shared<RtspDigestAuthenticator>(provider, "packetia", 30, true, [&] { return now; });
        auth = std::make_unique<AuthService>(digest, std::make_shared<StreamAccessPolicy>(), [&] { return now; });
        nonce = Nonce(digest->Challenge());
    }
    AuthResult Verify(const std::string& header, std::string_view method = "ANNOUNCE",
                      std::string_view uri = "rtsp://host/live/camera")
    { return auth->Authenticate(AuthRequest{header, method, uri}, Audience::Rtsp); }
    int64_t now = 100;
    std::string nonce;
    std::shared_ptr<StaticRtspCredentialProvider> provider;
    std::shared_ptr<RtspDigestAuthenticator> digest;
    std::unique_ptr<AuthService> auth;
};

TEST_F(RtspDigestTest, VerifiedIdentityUsesSharedServiceAndExactStreamPermissions)
{
    const auto result = Verify(Header(nonce));
    ASSERT_TRUE(result.Succeeded());
    EXPECT_EQ(result.context->subject_id, "alice");
    EXPECT_EQ(result.context->audience, Audience::Rtsp);
    ResourceContext stream;
    stream.type = ResourceType::Stream; stream.id = "live/camera";
    EXPECT_TRUE(auth->Authorize(*result.context, Action::Publish, stream).Allowed());
    EXPECT_FALSE(auth->Authorize(*result.context, Action::Subscribe, stream).Allowed());
    stream.id = "live/camera/other";
    EXPECT_FALSE(auth->Authorize(*result.context, Action::Publish, stream).Allowed());
    stream.id = "live/view";
    EXPECT_TRUE(auth->Authorize(*result.context, Action::Subscribe, stream).Allowed());
    auto wrong = *result.context; wrong.audience = Audience::WebRtc;
    EXPECT_FALSE(auth->Authorize(wrong, Action::Subscribe, stream).Allowed());
    EXPECT_FALSE(auth->Authenticate(Header(nonce), Audience::Rtsp).Succeeded());
}

TEST_F(RtspDigestTest, RejectsReplayAndAllowsOnlyIncreasingAuthenticatedNonceCounts)
{
    ASSERT_TRUE(Verify(Header(nonce)).Succeeded());
    EXPECT_FALSE(Verify(Header(nonce)).Succeeded());
    EXPECT_FALSE(Verify(Header(nonce, "ANNOUNCE", "rtsp://host/live/camera", "00000003", true, "wrong")).Succeeded());
    EXPECT_TRUE(Verify(Header(nonce, "ANNOUNCE", "rtsp://host/live/camera", "00000002")).Succeeded());
    EXPECT_FALSE(Verify(Header(nonce)).Succeeded());
}

TEST_F(RtspDigestTest, BindsDigestToMethodUriAudienceAndConnection)
{
    EXPECT_FALSE(Verify(Header(nonce), "RECORD").Succeeded());
    EXPECT_FALSE(Verify(Header(nonce), "ANNOUNCE", "rtsp://host/live/other").Succeeded());
    const auto header = Header(nonce);
    EXPECT_FALSE(auth->Authenticate(AuthRequest{header, "ANNOUNCE", "rtsp://host/live/camera"}, Audience::WebRtc).Succeeded());
    RtspDigestAuthenticator other(provider, "packetia", 30, true, [&] { return now; });
    EXPECT_NE(Nonce(other.Challenge()), nonce);
    EXPECT_FALSE(other.Authenticate(AuthRequest{header, "ANNOUNCE", "rtsp://host/live/camera"}, Audience::Rtsp).Succeeded());
    EXPECT_TRUE(Verify(header).Succeeded());
}

TEST_F(RtspDigestTest, ExpiredNonceRequiresFreshChallenge)
{
    now = 130;
    EXPECT_EQ(Verify(Header(nonce)).error, AuthError::ExpiredToken);
    const auto challenge = digest->Challenge(true);
    EXPECT_NE(challenge.find("stale=true"), std::string::npos);
    const auto next = Nonce(challenge);
    ASSERT_NE(next, nonce);
    EXPECT_FALSE(Verify(Header(nonce)).Succeeded());
    EXPECT_TRUE(Verify(Header(next)).Succeeded());
}

TEST_F(RtspDigestTest, LegacyDigestNonceIsSingleUseAndCanBeDisabled)
{
    const auto legacy = Header(nonce, "ANNOUNCE", "rtsp://host/live/camera", "", false);
    EXPECT_TRUE(Verify(legacy).Succeeded());
    EXPECT_EQ(Verify(legacy).error, AuthError::ExpiredToken);
    const auto next = Nonce(digest->Challenge(true));
    EXPECT_NE(next, nonce);
    EXPECT_TRUE(Verify(Header(next, "ANNOUNCE", "rtsp://host/live/camera", "", false)).Succeeded());
    RtspDigestAuthenticator strict(provider, "packetia", 30, false, [&] { return now; });
    const auto strict_nonce = Nonce(strict.Challenge());
    const auto header = Header(strict_nonce, "ANNOUNCE", "rtsp://host/live/camera", "", false);
    EXPECT_FALSE(strict.Authenticate(AuthRequest{header, "ANNOUNCE", "rtsp://host/live/camera"}, Audience::Rtsp).Succeeded());
}

TEST_F(RtspDigestTest, RejectsAmbiguousMalformedAndUnsupportedCredentials)
{
    const auto valid = Header(nonce);
    for (const auto& invalid : {valid + ", username=\"alice\"", valid + ", algorithm=SHA-256",
        valid + ",", valid + "\r\nInjected: value", std::string("Basic YWxpY2U6c2VjcmV0"),
        Header(nonce, "ANNOUNCE", "rtsp://host/live/camera", "00000000"), std::string(8193, 'a')})
        EXPECT_FALSE(Verify(invalid).Succeeded());
    EXPECT_TRUE(Verify(valid).Succeeded());
}

TEST(RtspAuthOptions, RejectsInvalidCredentialsAndAmbiguousPaths)
{
    RtspAuthOptions options; options.enabled = true;
    EXPECT_THROW(options.Validate(), std::invalid_argument);
    options.users = {{"alice", Md5("alice:packetia:secret"), {{"live/camera", true, false}}}};
    EXPECT_NO_THROW(options.Validate());
    for (const auto* path : {"/live/camera", "live/../camera", "live//camera", "live/camera/", "live/%2e", "live/camera?x", "*"})
    { options.users[0].streams[0].path = path; EXPECT_THROW(options.Validate(), std::invalid_argument); }
}
}
}
