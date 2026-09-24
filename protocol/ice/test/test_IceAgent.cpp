#include <gtest/gtest.h>
#include <algorithm>

#include "IceAgent.h"

namespace ice
{

namespace
{
std::vector<uint8_t> Request(bool nominate = true, const std::string& username = "local:remote",
                             const std::string& password = "local_pwd", bool controlling = true)
{
    protocol::IceRequestParams params;
    params.username = username;
    params.password = password;
    params.priority = 1234;
    params.controlling = controlling;
    params.tie_breaker = 42;
    params.use_candidate = nominate;
    std::vector<uint8_t> packet(1500);
    size_t size = 0;
    EXPECT_TRUE(protocol::StunCodec::BuildIceBindingRequest(params, packet.data(), packet.size(), size));
    packet.resize(size);
    return packet;
}

IceAgent::HandleResult Deliver(IceAgent& agent, const std::vector<uint8_t>& packet,
                              std::vector<uint8_t>& response, uint16_t port = 50000)
{
    return agent.HandleDatagram(network::SocketAddr::FromIPPort("192.0.2.10", port),
                                packet.data(), packet.size(), response);
}

void RefreshFingerprint(std::vector<uint8_t>& packet)
{
    const auto length = packet.size() - 20;
    packet[2] = static_cast<uint8_t>(length >> 8);
    packet[3] = static_cast<uint8_t>(length);
    const auto crc = protocol::StunCodec::ComputeFingerprint(packet.data(), packet.size() - 8);
    for (int i = 0; i < 4; ++i) packet[packet.size() - 4 + i] = crc >> (24 - 8 * i);
}

void Resign(std::vector<uint8_t>& packet)
{
    RefreshFingerprint(packet);
    protocol::StunMessageInfo msg;
    ASSERT_TRUE(protocol::StunCodec::Parse(packet.data(), packet.size(), msg));
    const auto* mi = msg.FindAttr(static_cast<uint16_t>(protocol::AttrType::MESSAGE_INTEGRITY));
    ASSERT_NE(mi, nullptr);
    const size_t offset = mi->value_offset;
    const size_t length = offset; // MI value end minus the 20-byte STUN header.
    packet[2] = length >> 8;
    packet[3] = length;
    uint8_t digest[20];
    ASSERT_TRUE(protocol::StunCodec::ComputeMessageIntegrity(packet.data(), offset - 4, "local_pwd", digest));
    std::copy(digest, digest + 20, packet.begin() + offset);
    RefreshFingerprint(packet);
}
} // namespace

TEST(StunInteropTest, Rfc5769Request)
{
    // RFC 5769 section 2.1, independent of this project's packet builder.
    const uint8_t packet[] = {
        0x00,0x01,0x00,0x58,0x21,0x12,0xa4,0x42,0xb7,0xe7,0xa7,0x01,
        0xbc,0x34,0xd6,0x86,0xfa,0x87,0xdf,0xae,0x80,0x22,0x00,0x10,
        0x53,0x54,0x55,0x4e,0x20,0x74,0x65,0x73,0x74,0x20,0x63,0x6c,
        0x69,0x65,0x6e,0x74,0x00,0x24,0x00,0x04,0x6e,0x00,0x01,0xff,
        0x80,0x29,0x00,0x08,0x93,0x2f,0xf9,0xb1,0x51,0x26,0x3b,0x36,
        0x00,0x06,0x00,0x09,0x65,0x76,0x74,0x6a,0x3a,0x68,0x36,0x76,
        0x59,0x20,0x20,0x20,0x00,0x08,0x00,0x14,0x9a,0xea,0xa7,0x0c,
        0xbf,0xd8,0xcb,0x56,0x78,0x1e,0xf2,0xb5,0xb2,0xd3,0xf2,0x49,
        0xc1,0xb5,0x71,0xa2,0x80,0x28,0x00,0x04,0xe5,0x7a,0x3b,0xcf
    };
    protocol::StunMessageInfo msg;
    ASSERT_TRUE(protocol::StunCodec::Parse(packet, sizeof(packet), msg));
    EXPECT_TRUE(protocol::StunCodec::VerifyFingerprint(msg));
    EXPECT_TRUE(protocol::StunCodec::VerifyMessageIntegrity(msg, "VOkJxbRl1RmTxUk/WvJxBt"));
    EXPECT_FALSE(protocol::StunCodec::VerifyMessageIntegrity(msg, "wrong"));
}

TEST(IceLivenessTest, InitialChecksDoNotPostponeNominationDeadline)
{
    uint64_t now = 0;
    IceAgent agent([&] { return now; });
    agent.SetLocalCredentials("local", "local_pwd");
    EXPECT_FALSE(agent.StartLiveness(0));
    ASSERT_TRUE(agent.StartLiveness(100));
    std::vector<uint8_t> response;
    now = 99;
    EXPECT_EQ(Deliver(agent, Request(false), response), IceAgent::HandleResult::SuccessResponse);
    EXPECT_TRUE(agent.CheckLiveness());
    now = 100;
    EXPECT_FALSE(agent.CheckLiveness());
    EXPECT_EQ(Deliver(agent, Request(), response), IceAgent::HandleResult::Ignored);
    EXPECT_TRUE(response.empty());
    EXPECT_FALSE(agent.HasSelectedPeer());
}

TEST(IceStateTest, ChecksNominationCredentialChangeAndClose)
{
    IceAgent agent;
    EXPECT_EQ(agent.CurrentState(), IceAgent::State::New);
    agent.SetLocalCredentials("local", "local_pwd");
    std::vector<uint8_t> response;
    Deliver(agent, Request(true, "local:remote", "wrong"), response);
    EXPECT_EQ(agent.CurrentState(), IceAgent::State::New);
    Deliver(agent, Request(false), response);
    EXPECT_EQ(agent.CurrentState(), IceAgent::State::Checking);
    agent.SetOnSelectedPeer([&](const auto& peer) {
        EXPECT_EQ(agent.CurrentState(), IceAgent::State::Completed);
        EXPECT_TRUE(agent.HasSelectedPeer());
        EXPECT_TRUE(agent.SelectedPeer() == peer);
    });
    Deliver(agent, Request(), response);
    EXPECT_EQ(agent.CurrentState(), IceAgent::State::Completed);
    agent.SetLocalCredentials("next", "next_pwd");
    EXPECT_EQ(agent.CurrentState(), IceAgent::State::Checking);
    EXPECT_FALSE(agent.HasSelectedPeer());
    EXPECT_EQ(agent.SelectedPeer().len, 0);
    agent.Close();
    agent.Close();
    EXPECT_EQ(agent.CurrentState(), IceAgent::State::Closed);
    EXPECT_FALSE(agent.StartLiveness(100));
    agent.SetLocalCredentials("local", "local_pwd");
    EXPECT_EQ(Deliver(agent, Request(), response), IceAgent::HandleResult::Ignored);
    EXPECT_TRUE(response.empty());
    EXPECT_EQ(agent.CurrentState(), IceAgent::State::Closed);
}

TEST(IceStateTest, TimeoutRequiresExplicitRestartAndCloseIsTerminal)
{
    uint64_t now = 0;
    IceAgent agent([&] { return now; });
    agent.SetLocalCredentials("local", "local_pwd");
    ASSERT_TRUE(agent.StartLiveness(100));
    EXPECT_EQ(agent.CurrentState(), IceAgent::State::Checking);
    now = 100;
    EXPECT_FALSE(agent.CheckLiveness());
    EXPECT_EQ(agent.CurrentState(), IceAgent::State::Failed);
    agent.SetRemoteCredentials("remote", "remote_pwd");
    EXPECT_EQ(agent.CurrentState(), IceAgent::State::Failed);
    EXPECT_TRUE(agent.StartLiveness(100));
    EXPECT_EQ(agent.CurrentState(), IceAgent::State::Checking);
    agent.StopLiveness();
    EXPECT_EQ(agent.CurrentState(), IceAgent::State::Closed);
    EXPECT_FALSE(agent.CheckLiveness());
}

TEST(IceLivenessTest, OnlyAuthenticatedSelectedTupleRefreshesDeadline)
{
    for (int kind = 0; kind < 3; ++kind)
    {
        uint64_t now = 0;
        IceAgent agent([&] { return now; });
        agent.SetLocalCredentials("local", "local_pwd");
        ASSERT_TRUE(agent.StartLiveness(100));
        std::vector<uint8_t> response;
        now = 10;
        ASSERT_EQ(Deliver(agent, Request(), response), IceAgent::HandleResult::SuccessResponse);
        now = 90;
        if (kind == 0) Deliver(agent, Request(false), response);
        if (kind == 1) Deliver(agent, Request(false, "local:remote", "wrong"), response);
        if (kind == 2) Deliver(agent, Request(false), response, 50001);
        now = 110;
        EXPECT_EQ(agent.CheckLiveness(), kind == 0);
        now = 190;
        EXPECT_FALSE(agent.CheckLiveness());
        EXPECT_FALSE(agent.HasSelectedPeer());
    }
}

TEST(IceLivenessTest, RenominationRefreshesNewTupleButOldTupleCannotRefresh)
{
    uint64_t now = 0;
    IceAgent agent([&] { return now; });
    agent.SetLocalCredentials("local", "local_pwd");
    ASSERT_TRUE(agent.StartLiveness(100));
    std::vector<uint8_t> response;
    ASSERT_EQ(Deliver(agent, Request(), response), IceAgent::HandleResult::SuccessResponse);
    now = 80;
    ASSERT_EQ(Deliver(agent, Request(), response, 50001), IceAgent::HandleResult::SuccessResponse);
    now = 150;
    Deliver(agent, Request(false), response);
    now = 180;
    EXPECT_FALSE(agent.CheckLiveness());
    EXPECT_FALSE(agent.CheckLiveness());
    ASSERT_TRUE(agent.StartLiveness(100));
    EXPECT_FALSE(agent.HasSelectedPeer());
    EXPECT_TRUE(agent.CheckLiveness());
}

TEST(IceAgentTest, CheckThenNominationIsIdempotentAndCanSelectNewTuple)
{
    IceAgent agent;
    agent.SetLocalCredentials("local", "local_pwd");
    int selected = 0;
    agent.SetOnSelectedPeer([&](const auto&) { ++selected; });
    std::vector<uint8_t> response;
    EXPECT_EQ(Deliver(agent, Request(false), response), IceAgent::HandleResult::SuccessResponse);
    EXPECT_FALSE(agent.HasSelectedPeer());
    EXPECT_EQ(Deliver(agent, Request(), response), IceAgent::HandleResult::SuccessResponse);
    EXPECT_EQ(Deliver(agent, Request(), response), IceAgent::HandleResult::SuccessResponse);
    EXPECT_EQ(selected, 1);
    EXPECT_EQ(Deliver(agent, Request(false), response, 50001), IceAgent::HandleResult::SuccessResponse);
    EXPECT_EQ(agent.SelectedPeer().Port(), 50000);
    EXPECT_EQ(Deliver(agent, Request(), response, 50001), IceAgent::HandleResult::SuccessResponse);
    EXPECT_EQ(selected, 2);
    EXPECT_EQ(agent.SelectedPeer().Port(), 50001);
}

TEST(IceAgentTest, FailedAuthenticationCannotPoisonRemoteCredentials)
{
    IceAgent agent;
    agent.SetLocalCredentials("local", "local_pwd");
    std::vector<uint8_t> response;
    EXPECT_EQ(Deliver(agent, Request(true, "local:attacker", "wrong"), response),
              IceAgent::HandleResult::ErrorResponse);
    EXPECT_TRUE(agent.Context().RemoteUfrag().empty());
    EXPECT_FALSE(agent.HasSelectedPeer());
    EXPECT_EQ(Deliver(agent, Request(), response), IceAgent::HandleResult::SuccessResponse);
    EXPECT_EQ(agent.Context().RemoteUfrag(), "remote");
}

TEST(IceAgentTest, UnauthenticatedSuffixCannotNominate)
{
    IceAgent agent;
    agent.SetLocalCredentials("local", "local_pwd");
    auto packet = Request(false);
    // Append USE-CANDIDATE after the authenticated region, before fingerprint.
    packet.insert(packet.end() - 8, {0x00, 0x25, 0x00, 0x00});
    RefreshFingerprint(packet);
    std::vector<uint8_t> response;
    EXPECT_EQ(Deliver(agent, packet, response), IceAgent::HandleResult::SuccessResponse);
    EXPECT_FALSE(agent.HasSelectedPeer());
}

TEST(IceAgentTest, LiteRoleConflictDoesNotSelectPeer)
{
    IceAgent agent;
    agent.SetLocalCredentials("local", "local_pwd");
    std::vector<uint8_t> response;
    EXPECT_EQ(Deliver(agent, Request(false, "local:remote", "local_pwd", false), response),
              IceAgent::HandleResult::ErrorResponse);
    protocol::StunMessageInfo msg;
    ASSERT_TRUE(protocol::StunCodec::Parse(response.data(), response.size(), msg));
    protocol::StunErrorCode err;
    ASSERT_TRUE(protocol::StunCodec::DecodeErrorCode(msg, err));
    EXPECT_EQ(err.code, 487);
    EXPECT_TRUE(protocol::StunCodec::VerifyMessageIntegrity(msg, "local_pwd"));
    EXPECT_FALSE(agent.HasSelectedPeer());
}

TEST(IceAgentTest, ChangedCredentialsInvalidateSelectionAndOldRequests)
{
    IceAgent agent;
    agent.SetLocalCredentials("local", "local_pwd");
    std::vector<uint8_t> response;
    ASSERT_EQ(Deliver(agent, Request(), response), IceAgent::HandleResult::SuccessResponse);
    agent.SetLocalCredentials("newlocal", "new_pwd");
    EXPECT_FALSE(agent.HasSelectedPeer());
    EXPECT_EQ(Deliver(agent, Request(), response), IceAgent::HandleResult::ErrorResponse);
    EXPECT_EQ(Deliver(agent, Request(true, "newlocal:remote", "new_pwd"), response),
              IceAgent::HandleResult::SuccessResponse);
    agent.SetRemoteCredentials("newremote", "remote_pwd");
    EXPECT_FALSE(agent.HasSelectedPeer());
}

TEST(IceAgentTest, CorruptFingerprintAndTrailingDatagramAreDiscarded)
{
    IceAgent agent;
    agent.SetLocalCredentials("local", "local_pwd");
    std::vector<uint8_t> response;
    auto packet = Request();
    packet.back() ^= 1;
    EXPECT_EQ(Deliver(agent, packet, response), IceAgent::HandleResult::Ignored);
    EXPECT_TRUE(response.empty());
    packet = Request();
    packet.push_back(0);
    EXPECT_EQ(Deliver(agent, packet, response), IceAgent::HandleResult::Ignored);
    EXPECT_FALSE(agent.HasSelectedPeer());
}

TEST(IceAgentTest, UnknownRequiredAttributeGetsAuthenticated420)
{
    IceAgent agent;
    agent.SetLocalCredentials("local", "local_pwd");
    auto packet = Request();
    packet.insert(packet.end() - 32, {0x00, 0x7f, 0x00, 0x00});
    Resign(packet);
    std::vector<uint8_t> response;
    EXPECT_EQ(Deliver(agent, packet, response), IceAgent::HandleResult::ErrorResponse);
    protocol::StunMessageInfo msg;
    ASSERT_TRUE(protocol::StunCodec::Parse(response.data(), response.size(), msg));
    protocol::StunErrorCode error;
    ASSERT_TRUE(protocol::StunCodec::DecodeErrorCode(msg, error));
    EXPECT_EQ(error.code, 420);
    const auto* unknown = msg.FindAttr(static_cast<uint16_t>(protocol::AttrType::UNKNOWN_ATTRIBUTES));
    ASSERT_NE(unknown, nullptr);
    EXPECT_EQ(msg.AttrValue(*unknown), std::string("\x00\x7f", 2));
    EXPECT_TRUE(protocol::StunCodec::VerifyMessageIntegrity(msg, "local_pwd"));
    EXPECT_FALSE(agent.HasSelectedPeer());
}

TEST(IceAgentTest, IPv6NominationReturnsMappedAddress)
{
    IceAgent agent;
    agent.SetLocalCredentials("local", "local_pwd");
    sockaddr_in6 addr{};
    addr.sin6_family = AF_INET6;
    addr.sin6_port = htons(55000);
    ASSERT_EQ(inet_pton(AF_INET6, "2001:db8::1234", &addr.sin6_addr), 1);
    const auto peer = network::SocketAddr::FromSockaddr(
        reinterpret_cast<const sockaddr*>(&addr), sizeof(addr));
    const auto packet = Request();
    std::vector<uint8_t> response;
    ASSERT_EQ(agent.HandleDatagram(peer, packet.data(), packet.size(), response),
              IceAgent::HandleResult::SuccessResponse);
    protocol::StunMessageInfo msg;
    ASSERT_TRUE(protocol::StunCodec::Parse(response.data(), response.size(), msg));
    protocol::XorMappedAddress mapped;
    ASSERT_TRUE(protocol::StunCodec::DecodeXorMappedAddress(msg, mapped));
    EXPECT_TRUE(mapped.is_ipv6);
    EXPECT_EQ(mapped.port, 55000);
    EXPECT_EQ(mapped.ip[0], 0x20);
    EXPECT_EQ(mapped.ip[1], 0x01);
    EXPECT_EQ(mapped.ip[14], 0x12);
    EXPECT_EQ(mapped.ip[15], 0x34);
    EXPECT_TRUE(protocol::StunCodec::VerifyMessageIntegrity(msg, "local_pwd"));
    EXPECT_TRUE(agent.SelectedPeer() == peer);
}

TEST(IceAgentTest, MissingIntegrityGetsUnsigned400)
{
    IceAgent agent;
    agent.SetLocalCredentials("local", "local_pwd");
    auto packet = Request();
    packet.erase(packet.end() - 32, packet.end() - 8);
    RefreshFingerprint(packet);
    std::vector<uint8_t> response;
    EXPECT_EQ(Deliver(agent, packet, response), IceAgent::HandleResult::ErrorResponse);
    protocol::StunMessageInfo msg;
    ASSERT_TRUE(protocol::StunCodec::Parse(response.data(), response.size(), msg));
    protocol::StunErrorCode error;
    ASSERT_TRUE(protocol::StunCodec::DecodeErrorCode(msg, error));
    EXPECT_EQ(error.code, 400);
    EXPECT_FALSE(msg.HasAttr(static_cast<uint16_t>(protocol::AttrType::MESSAGE_INTEGRITY)));
    EXPECT_FALSE(agent.HasSelectedPeer());
}

TEST(IceAgentTest, MissingPriorityAndConflictingRolesCannotNominate)
{
    for (bool conflicting_roles : {false, true})
    {
        IceAgent agent;
        agent.SetLocalCredentials("local", "local_pwd");
        auto packet = Request();
        if (conflicting_roles)
            packet.insert(packet.end() - 32, {0x80, 0x29, 0x00, 0x00}); // malformed second role
        else
        {
            protocol::StunMessageInfo msg;
            ASSERT_TRUE(protocol::StunCodec::Parse(packet.data(), packet.size(), msg));
            const auto* attr = msg.FindAttr(static_cast<uint16_t>(protocol::AttrType::PRIORITY));
            ASSERT_NE(attr, nullptr);
            packet.erase(packet.begin() + attr->value_offset - 4, packet.begin() + attr->value_offset + 4);
        }
        Resign(packet);
        std::vector<uint8_t> response;
        EXPECT_EQ(Deliver(agent, packet, response), IceAgent::HandleResult::ErrorResponse);
        protocol::StunMessageInfo msg;
        ASSERT_TRUE(protocol::StunCodec::Parse(response.data(), response.size(), msg));
        protocol::StunErrorCode error;
        ASSERT_TRUE(protocol::StunCodec::DecodeErrorCode(msg, error));
        EXPECT_EQ(error.code, 400);
        EXPECT_FALSE(agent.HasSelectedPeer());
    }
}

TEST(IceAgentTest, BindingRequestBuildsSuccessAndSelectsPeer)
{
    constexpr const char* kLocalUfrag = "local";
    constexpr const char* kLocalPwd = "local_pwd";
    constexpr const char* kRemoteUfrag = "remote";

    IceAgent agent;
    agent.SetLocalCredentials(kLocalUfrag, kLocalPwd);
    agent.SetRemoteCredentials(kRemoteUfrag, "remote_pwd");

    protocol::IceRequestParams req;
    req.txid = {0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11};
    req.username = std::string(kLocalUfrag) + ":" + kRemoteUfrag;
    req.priority = 1234;
    req.controlling = true;
    req.tie_breaker = 0x1122334455667788ull;
    req.use_candidate = true;
    req.password = kLocalPwd;

    uint8_t request_buf[1500] = {0};
    size_t request_len = 0;
    ASSERT_TRUE(protocol::StunCodec::BuildIceBindingRequest(
        req, request_buf, sizeof(request_buf), request_len));

    auto peer = network::SocketAddr::FromIPPort("192.0.2.10", 50000);

    std::vector<uint8_t> response;
    auto result = agent.HandleDatagram(peer, request_buf, request_len, response);
    EXPECT_EQ(result, IceAgent::HandleResult::SuccessResponse);
    EXPECT_TRUE(agent.HasSelectedPeer());
    EXPECT_TRUE(agent.SelectedPeer() == peer);
    ASSERT_FALSE(response.empty());

    protocol::StunMessageInfo rsp;
    ASSERT_TRUE(protocol::StunCodec::Parse(response.data(), response.size(), rsp));
    EXPECT_TRUE(rsp.IsBindingResponse());
    EXPECT_TRUE(protocol::StunCodec::VerifyMessageIntegrity(rsp, kLocalPwd));
    EXPECT_TRUE(protocol::StunCodec::VerifyFingerprint(rsp));

    protocol::XorMappedAddress mapped;
    ASSERT_TRUE(protocol::StunCodec::DecodeXorMappedAddress(rsp, mapped));
    EXPECT_FALSE(mapped.is_ipv6);
    EXPECT_EQ(mapped.port, 50000);
    EXPECT_EQ(mapped.ip[0], 192);
    EXPECT_EQ(mapped.ip[1], 0);
    EXPECT_EQ(mapped.ip[2], 2);
    EXPECT_EQ(mapped.ip[3], 10);
}

TEST(IceAgentTest, BadUsernameBuildsUnauthorizedError)
{
    IceAgent agent;
    agent.SetLocalCredentials("local", "local_pwd");

    protocol::IceRequestParams req;
    req.txid = {11, 10, 9, 8, 7, 6, 5, 4, 3, 2, 1, 0};
    req.username = "other:remote";
    req.priority = 1;
    req.controlling = true;
    req.tie_breaker = 1;
    req.password = "local_pwd";

    uint8_t request_buf[1500] = {0};
    size_t request_len = 0;
    ASSERT_TRUE(protocol::StunCodec::BuildIceBindingRequest(
        req, request_buf, sizeof(request_buf), request_len));

    auto peer = network::SocketAddr::FromIPPort("192.0.2.20", 50001);

    std::vector<uint8_t> response;
    auto result = agent.HandleDatagram(peer, request_buf, request_len, response);
    EXPECT_EQ(result, IceAgent::HandleResult::ErrorResponse);
    ASSERT_FALSE(response.empty());

    protocol::StunMessageInfo rsp;
    ASSERT_TRUE(protocol::StunCodec::Parse(response.data(), response.size(), rsp));
    EXPECT_TRUE(rsp.IsBindingErrorResponse());

    protocol::StunErrorCode err;
    ASSERT_TRUE(protocol::StunCodec::DecodeErrorCode(rsp, err));
    EXPECT_EQ(err.code, 401);
}

} // namespace ice
