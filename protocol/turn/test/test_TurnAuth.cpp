#include <gtest/gtest.h>

#include <utility>
#include "TurnAuth.h"

namespace protocol
{
namespace
{
const std::array<uint8_t, 12> kTransaction = {1,2,3,4,5,6,7,8,9,10,11,12};

StunAttribute Text(AttrType type, std::string_view value)
{
    return {static_cast<uint16_t>(type), {value.begin(), value.end()}};
}

std::string Nonce(const TurnAuthResult& result)
{
    StunMessageInfo msg;
    if (!TurnCodec::ParseStunDatagram(result.response.data(), result.response.size(), msg)) return {};
    const auto* attr = msg.FindAttr(static_cast<uint16_t>(AttrType::NONCE));
    return attr ? std::string(msg.AttrValue(*attr)) : std::string{};
}

class TurnAuthTest : public ::testing::Test
{
protected:
    uint64_t now = 0;
    int lookups = 0;
    int nonces = 0;
    bool random_failure = false;
    TurnAuthContext context;
    TurnAuth auth{
        "example.org",
        [this](std::string_view username, std::string& password) {
            ++lookups;
            if (username != "alice") return false;
            password = "secret";
            return true;
        },
        1000,
        [this] { return now; },
        [this](std::string& nonce) {
            if (random_failure) return false;
            nonce = "test-nonce-" + std::to_string(++nonces);
            return true;
        }
    };

    std::vector<uint8_t> Unsigned(StunMethod method = StunMethod::Allocate)
    {
        std::vector<uint8_t> packet;
        EXPECT_TRUE(StunCodec::BuildMessage(method, StunClass::Request, kTransaction,
            {TurnCodec::RequestedTransportAttribute(17)}, packet));
        return packet;
    }

    std::vector<uint8_t> Signed(std::string_view nonce, std::string_view username = "alice",
                                std::string_view password = "secret", std::string_view realm = "example.org",
                                bool fingerprint = true, StunMethod method = StunMethod::Allocate)
    {
        std::string key;
        EXPECT_TRUE(TurnAuth::DerivePreparedLegacyKey(username, realm, password, key));
        std::vector<uint8_t> packet;
        EXPECT_TRUE(StunCodec::BuildMessage(method, StunClass::Request, kTransaction,
            {Text(AttrType::USERNAME, username), Text(AttrType::REALM, realm), Text(AttrType::NONCE, nonce),
             TurnCodec::RequestedTransportAttribute(17)}, packet, key, fingerprint));
        return packet;
    }

    TurnAuthResult Deliver(const std::vector<uint8_t>& packet)
    {
        return auth.Authenticate(packet.data(), packet.size(), context);
    }

    void ExpectError(const TurnAuthResult& result, uint16_t code, StunMethod method = StunMethod::Allocate)
    {
        ASSERT_EQ(result.status, TurnAuthResult::Status::Response);
        EXPECT_EQ(result.error_code, code);
        EXPECT_TRUE(result.username.empty());
        EXPECT_TRUE(result.integrity_key.empty());
        StunMessageInfo msg;
        ASSERT_TRUE(TurnCodec::ParseStunDatagram(result.response.data(), result.response.size(), msg));
        EXPECT_EQ(msg.klass, StunClass::ErrorResponse);
        EXPECT_EQ(msg.method, method);
        EXPECT_EQ(msg.txid, kTransaction);
        EXPECT_TRUE(StunCodec::VerifyFingerprint(msg));
        EXPECT_FALSE(msg.HasAttr(static_cast<uint16_t>(AttrType::MESSAGE_INTEGRITY)));
        StunErrorCode error;
        ASSERT_TRUE(StunCodec::DecodeErrorCode(msg, error));
        EXPECT_EQ(error.code, code);
        if (code == 401 || code == 438)
        {
            const auto* realm = msg.FindAttr(static_cast<uint16_t>(AttrType::REALM));
            ASSERT_NE(realm, nullptr);
            EXPECT_EQ(msg.AttrValue(*realm), "example.org");
            EXPECT_FALSE(Nonce(result).empty());
        }
        else
        {
            EXPECT_FALSE(msg.HasAttr(static_cast<uint16_t>(AttrType::REALM)));
            EXPECT_FALSE(msg.HasAttr(static_cast<uint16_t>(AttrType::NONCE)));
        }
    }

    std::string Challenge()
    {
        const auto result = Deliver(Unsigned());
        ExpectError(result, 401);
        return Nonce(result);
    }
};
} // namespace

TEST(TurnAuthKeyTest, Md5KeyMatchesRfc8489Example)
{
    std::string key;
    ASSERT_TRUE(TurnAuth::DerivePreparedLegacyKey("user", "realm", "pass", key));
    const uint8_t expected[] = {
        0x84,0x93,0xfb,0xc5,0x3b,0xa5,0x82,0xfb,0x4c,0x04,0x4c,0x45,0x6b,0xdc,0x40,0xeb
    };
    EXPECT_EQ(key, std::string(reinterpret_cast<const char*>(expected), sizeof(expected)));
}

TEST(TurnAuthKeyTest, PreparedKeyAndBuilderMatchRfc5769LongTermRequest)
{
    // RFC 5769 section 2.4. The helper consumes already prepared credentials;
    // the ASCII-only TURN request authenticator does not normalize this name.
    const uint8_t packet[] = {
        0x00,0x01,0x00,0x60,0x21,0x12,0xa4,0x42,
        0x78,0xad,0x34,0x33,0xc6,0xad,0x72,0xc0,0x29,0xda,0x41,0x2e,
        0x00,0x06,0x00,0x12,0xe3,0x83,0x9e,0xe3,0x83,0x88,0xe3,0x83,
        0xaa,0xe3,0x83,0x83,0xe3,0x82,0xaf,0xe3,0x82,0xb9,0x00,0x00,
        0x00,0x15,0x00,0x1c,0x66,0x2f,0x2f,0x34,0x39,0x39,0x6b,0x39,
        0x35,0x34,0x64,0x36,0x4f,0x4c,0x33,0x34,0x6f,0x4c,0x39,0x46,
        0x53,0x54,0x76,0x79,0x36,0x34,0x73,0x41,
        0x00,0x14,0x00,0x0b,0x65,0x78,0x61,0x6d,0x70,0x6c,0x65,0x2e,
        0x6f,0x72,0x67,0x00,
        0x00,0x08,0x00,0x14,0xf6,0x70,0x24,0x65,0x6d,0xd6,0x4a,0x3e,
        0x02,0xb8,0xe0,0x71,0x2e,0x85,0xc9,0xa2,0x8c,0xa8,0x96,0x66
    };
    StunMessageInfo msg;
    ASSERT_TRUE(TurnCodec::ParseStunDatagram(packet, sizeof(packet), msg));
    const auto* username_attr = msg.FindAttr(static_cast<uint16_t>(AttrType::USERNAME));
    ASSERT_NE(username_attr, nullptr);
    const auto username = msg.AttrValue(*username_attr);
    std::string key;
    ASSERT_TRUE(TurnAuth::DerivePreparedLegacyKey(username, "example.org", "TheMatrIX", key));
    EXPECT_TRUE(StunCodec::VerifyMessageIntegrity(msg, key));
    std::vector<uint8_t> built;
    ASSERT_TRUE(StunCodec::BuildMessage(StunMethod::Binding, StunClass::Request, msg.txid,
        {Text(AttrType::USERNAME, username), Text(AttrType::NONCE, "f//499k954d6OL34oL9FSTvy64sA"),
         Text(AttrType::REALM, "example.org")}, built, key));
    EXPECT_EQ(built, std::vector<uint8_t>(packet, packet + sizeof(packet)));
}

TEST_F(TurnAuthTest, ChallengeRetryAuthorizesAndReturnsResponseSigningKey)
{
    const auto nonce = Challenge();
    EXPECT_EQ(nonces, 1);
    EXPECT_EQ(lookups, 0);
    const auto result = Deliver(Signed(nonce));
    ASSERT_EQ(result.status, TurnAuthResult::Status::Authorized);
    EXPECT_EQ(result.error_code, 0);
    EXPECT_TRUE(result.response.empty());
    EXPECT_EQ(result.username, "alice");
    EXPECT_EQ(result.integrity_key.size(), 16u);
    EXPECT_EQ(lookups, 1);
    std::vector<uint8_t> response;
    ASSERT_TRUE(StunCodec::BuildMessage(StunMethod::Allocate, StunClass::SuccessResponse, kTransaction,
        {TurnCodec::UInt32Attribute(AttrType::LIFETIME, 600)}, response, result.integrity_key, true));
    StunMessageInfo msg;
    ASSERT_TRUE(TurnCodec::ParseStunDatagram(response.data(), response.size(), msg));
    std::string client_key;
    ASSERT_TRUE(TurnAuth::DerivePreparedLegacyKey("alice", "example.org", "secret", client_key));
    EXPECT_TRUE(StunCodec::VerifyMessageIntegrity(msg, client_key));
}

TEST_F(TurnAuthTest, RepeatedUnsignedRequestsReuseNonceWithoutExtendingExpiry)
{
    const auto first = Challenge();
    now = 999;
    EXPECT_EQ(Challenge(), first);
    EXPECT_EQ(nonces, 1);
    now = 1000;
    EXPECT_NE(Challenge(), first);
    EXPECT_EQ(nonces, 2);
    EXPECT_EQ(lookups, 0);
}

TEST_F(TurnAuthTest, ExpiredNonceProduces438AndRetryWithNewNonceSucceeds)
{
    const auto old_nonce = Challenge();
    now = 999;
    EXPECT_EQ(Deliver(Signed(old_nonce)).status, TurnAuthResult::Status::Authorized);
    now = 1000;
    const auto stale = Deliver(Signed(old_nonce));
    ExpectError(stale, 438);
    const auto fresh_nonce = Nonce(stale);
    EXPECT_NE(fresh_nonce, old_nonce);
    EXPECT_EQ(lookups, 1); // A stale nonce never reaches password lookup.
    EXPECT_EQ(Deliver(Signed(fresh_nonce)).status, TurnAuthResult::Status::Authorized);
    EXPECT_EQ(nonces, 2);
}

TEST_F(TurnAuthTest, ContextsHaveSeparateNoncesAndCannotReuseEachOthersChallenge)
{
    const auto first = Challenge();
    TurnAuthContext other;
    const auto packet = Unsigned();
    const auto challenge = auth.Authenticate(packet.data(), packet.size(), other);
    ExpectError(challenge, 401);
    const auto second = Nonce(challenge);
    EXPECT_NE(first, second);
    const auto wrong = Deliver(Signed(second));
    ExpectError(wrong, 438);
    EXPECT_EQ(Nonce(wrong), first);
    EXPECT_EQ(nonces, 2);
    EXPECT_EQ(lookups, 0);
}

TEST_F(TurnAuthTest, WrongPasswordUnknownUserAndWrongRealmDoNotAuthorize)
{
    const auto nonce = Challenge();
    ExpectError(Deliver(Signed(nonce, "alice", "wrong")), 401);
    ExpectError(Deliver(Signed(nonce, "unknown")), 401);
    EXPECT_EQ(lookups, 2);
    ExpectError(Deliver(Signed(nonce, "alice", "secret", "other.org")), 401);
    EXPECT_EQ(lookups, 2);
    EXPECT_EQ(nonces, 1);
    EXPECT_EQ(Deliver(Signed(nonce)).status, TurnAuthResult::Status::Authorized);
}

TEST_F(TurnAuthTest, MissingAndDuplicateAuthenticatedCredentialsProduce400)
{
    const auto nonce = Challenge();
    std::string key;
    ASSERT_TRUE(TurnAuth::DerivePreparedLegacyKey("alice", "example.org", "secret", key));
    const std::vector<StunAttribute> credentials = {
        Text(AttrType::USERNAME, "alice"), Text(AttrType::REALM, "example.org"), Text(AttrType::NONCE, nonce)
    };
    for (size_t i = 0; i < credentials.size(); ++i)
    {
        auto missing = credentials;
        missing.erase(missing.begin() + i);
        auto duplicate = credentials;
        duplicate.push_back(credentials[i]);
        for (const auto& attributes : {missing, duplicate})
        {
            std::vector<uint8_t> packet;
            ASSERT_TRUE(StunCodec::BuildMessage(StunMethod::Allocate, StunClass::Request,
                                                kTransaction, attributes, packet, key, true));
            ExpectError(Deliver(packet), 400);
        }
    }
    EXPECT_EQ(lookups, 0);
}

TEST_F(TurnAuthTest, EmbeddedNullAndNonAsciiCredentialsAreRejected)
{
    const auto nonce = Challenge();
    const std::string usernames[] = {std::string("alice\0evil", 10), std::string("\xc3\xa9", 2)};
    for (const auto& username : usernames)
    {
        std::vector<uint8_t> packet;
        ASSERT_TRUE(StunCodec::BuildMessage(StunMethod::Allocate, StunClass::Request, kTransaction,
            {Text(AttrType::USERNAME, username), Text(AttrType::REALM, "example.org"), Text(AttrType::NONCE, nonce)},
            packet, "some-key"));
        ExpectError(Deliver(packet), 400);
    }
    EXPECT_EQ(lookups, 0);
}

TEST_F(TurnAuthTest, FingerprintIsOptionalButCorruptPacketsAreDiscarded)
{
    const auto nonce = Challenge();
    EXPECT_EQ(Deliver(Signed(nonce, "alice", "secret", "example.org", false)).status,
              TurnAuthResult::Status::Authorized);
    auto packet = Signed(nonce);
    packet.back() ^= 1;
    const auto bad = Deliver(packet);
    EXPECT_EQ(bad.status, TurnAuthResult::Status::Ignored);
    EXPECT_TRUE(bad.response.empty());
    EXPECT_EQ(lookups, 1);
}

TEST_F(TurnAuthTest, AllSupportedMethodsPreserveMethodInChallenges)
{
    const auto nonce = Challenge();
    for (auto method : {StunMethod::Allocate, StunMethod::Refresh,
                        StunMethod::CreatePermission, StunMethod::ChannelBind})
    {
        ExpectError(Deliver(Unsigned(method)), 401, method);
        EXPECT_EQ(Deliver(Signed(nonce, "alice", "secret", "example.org", true, method)).status,
                  TurnAuthResult::Status::Authorized);
    }
}

TEST_F(TurnAuthTest, IndicationsResponsesAndChannelDataCannotAuthorizeControlRequests)
{
    Challenge();
    for (auto klass : {StunClass::Indication, StunClass::SuccessResponse, StunClass::ErrorResponse})
    {
        std::vector<uint8_t> packet;
        ASSERT_TRUE(StunCodec::BuildMessage(StunMethod::Allocate, klass, kTransaction, {}, packet));
        EXPECT_EQ(Deliver(packet).status, TurnAuthResult::Status::Ignored);
    }
    EXPECT_EQ(Deliver(Unsigned(StunMethod::Binding)).status, TurnAuthResult::Status::Ignored);
    EXPECT_EQ(Deliver(Unsigned(StunMethod::Send)).status, TurnAuthResult::Status::Ignored);
    std::vector<uint8_t> packet;
    ASSERT_TRUE(TurnCodec::BuildChannelData(0x4000, "data", packet));
    EXPECT_EQ(Deliver(packet).status, TurnAuthResult::Status::Ignored);
    EXPECT_EQ(lookups, 0);
}

TEST_F(TurnAuthTest, CredentialsAppendedAfterIntegrityCannotSupplyMissingFields)
{
    const auto nonce = Challenge();
    std::string key;
    ASSERT_TRUE(TurnAuth::DerivePreparedLegacyKey("alice", "example.org", "secret", key));
    std::vector<uint8_t> packet, suffix;
    ASSERT_TRUE(StunCodec::BuildMessage(StunMethod::Allocate, StunClass::Request, kTransaction,
                                       {}, packet, key));
    ASSERT_TRUE(StunCodec::BuildMessage(StunMethod::Allocate, StunClass::Request, kTransaction,
        {Text(AttrType::USERNAME, "alice"), Text(AttrType::REALM, "example.org"), Text(AttrType::NONCE, nonce)}, suffix));
    packet.insert(packet.end(), suffix.begin() + 20, suffix.end());
    packet[2] = uint8_t((packet.size() - 20) >> 8);
    packet[3] = uint8_t(packet.size() - 20);
    StunMessageInfo msg;
    ASSERT_TRUE(TurnCodec::ParseStunDatagram(packet.data(), packet.size(), msg));
    ASSERT_TRUE(StunCodec::VerifyMessageIntegrity(msg, key));
    ExpectError(Deliver(packet), 400);
    EXPECT_EQ(lookups, 0);
}

TEST_F(TurnAuthTest, DuplicateIntegrityAndTruncatedDatagramsCannotAuthorize)
{
    const auto nonce = Challenge();
    auto packet = Signed(nonce, "alice", "secret", "example.org", false);
    StunMessageInfo msg;
    ASSERT_TRUE(TurnCodec::ParseStunDatagram(packet.data(), packet.size(), msg));
    const auto* attr = msg.FindAttr(static_cast<uint16_t>(AttrType::MESSAGE_INTEGRITY));
    ASSERT_NE(attr, nullptr);
    const std::vector<uint8_t> duplicate(packet.begin() + attr->value_offset - 4, packet.end());
    packet.insert(packet.end(), duplicate.begin(), duplicate.end());
    packet[2] = uint8_t((packet.size() - 20) >> 8);
    packet[3] = uint8_t(packet.size() - 20);
    ExpectError(Deliver(packet), 400);
    packet.pop_back();
    EXPECT_EQ(Deliver(packet).status, TurnAuthResult::Status::Ignored);
    EXPECT_EQ(lookups, 0);
}

TEST_F(TurnAuthTest, FailedNonceGenerationCannotAuthorizeOrSendInvalidChallenge)
{
    random_failure = true;
    const auto result = Deliver(Unsigned());
    EXPECT_EQ(result.status, TurnAuthResult::Status::InternalError);
    EXPECT_EQ(result.error_code, 500);
    EXPECT_TRUE(result.response.empty());
    EXPECT_TRUE(result.integrity_key.empty());
    random_failure = false;
    const auto nonce = Challenge();
    random_failure = true;
    now = 1000;
    EXPECT_EQ(Deliver(Signed(nonce)).status, TurnAuthResult::Status::InternalError);
    EXPECT_EQ(lookups, 0);
}

TEST(TurnAuthConfigurationTest, InvalidConfigurationFailsClosed)
{
    std::vector<uint8_t> packet;
    ASSERT_TRUE(StunCodec::BuildMessage(StunMethod::Allocate, StunClass::Request, kTransaction, {}, packet));
    const TurnAuth::PasswordLookup lookup = [](std::string_view, std::string&) { return false; };
    for (auto auth : {TurnAuth("", lookup), TurnAuth("example.org", {}), TurnAuth("example.org", lookup, 0)})
    {
        TurnAuthContext context;
        const auto result = auth.Authenticate(packet.data(), packet.size(), context);
        EXPECT_EQ(result.status, TurnAuthResult::Status::InternalError);
        EXPECT_TRUE(result.response.empty());
    }
}

TEST(TurnAuthNonceTest, DefaultGeneratorUsesDistinctRandomNonces)
{
    TurnAuth auth("example.org", [](std::string_view, std::string&) { return false; });
    TurnAuthContext first, second;
    std::vector<uint8_t> packet;
    ASSERT_TRUE(StunCodec::BuildMessage(StunMethod::Allocate, StunClass::Request, kTransaction, {}, packet));
    const auto a = auth.Authenticate(packet.data(), packet.size(), first);
    const auto b = auth.Authenticate(packet.data(), packet.size(), second);
    ASSERT_EQ(a.status, TurnAuthResult::Status::Response);
    ASSERT_EQ(b.status, TurnAuthResult::Status::Response);
    EXPECT_EQ(Nonce(a).size(), 64u);
    EXPECT_EQ(Nonce(b).size(), 64u);
    EXPECT_NE(Nonce(a), Nonce(b));
}

} // namespace protocol
