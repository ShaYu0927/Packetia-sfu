#include <gtest/gtest.h>

#include "IceAgent.h"

namespace ice
{

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
