#include "RtspServer.h"
#include "MediaStreamRegistry.h"
#include "WorkerRegistry.h"
#include "EndpointBase.h"
#include <gtest/gtest.h>
#include <openssl/evp.h>
#include <iomanip>
#include <sstream>
#include <sys/socket.h>
#include <unistd.h>

namespace
{
std::string Md5(const std::string& text)
{
    unsigned char bytes[16]; unsigned int size = 0;
    if (EVP_Digest(text.data(), text.size(), bytes, &size, EVP_md5(), nullptr) != 1 || size != 16) return {};
    std::ostringstream result;
    for (auto byte : bytes) result << std::hex << std::setw(2) << std::setfill('0') << unsigned(byte);
    return result.str();
}
std::string ExtractNonce(const std::string& response)
{
    auto pos = response.find("nonce=\"");
    if (pos == std::string::npos) return {};
    pos += 7;
    return response.substr(pos, response.find('"', pos) - pos);
}
const std::string kUri = "rtsp://127.0.0.1/live/auth";
const std::string kSdp = "v=0\r\no=- 1 1 IN IP4 127.0.0.1\r\ns=test\r\nt=0 0\r\n"
    "m=video 0 RTP/AVP 96\r\na=rtpmap:96 H264/90000\r\na=control:trackID=0\r\n";

class RtspAuthIntegration : public testing::Test
{
protected:
    void SetUp() override
    {
        baseline = utils::EndpointManager::Instance().Size();
        registry.Add({POOL_MEDIA, 2, 4096, ShardedWorkerPool::DropPolicy::DropHead},
            std::make_shared<utils::EndpointJobHandler>(&utils::EndpointManager::Instance()));
        ASSERT_EQ(registry.Start(), 0);
        ASSERT_TRUE(loop.Start());
        service::auth::RtspAuthOptions options;
        options.enabled = true;
        options.users = {
            {"alice", Md5("alice:packetia:secret"), {{"live/auth", true, false}, {"live/second", true, false}}},
            {"viewer", Md5("viewer:packetia:secret"), {{"live/auth", false, true}}}
        };
        server = std::make_shared<RtspServer>(&loop, options);
        ASSERT_TRUE(server->Start("127.0.0.1", 0));
        client = ::socket(AF_INET, SOCK_STREAM, 0);
        ASSERT_GE(client, 0);
        const auto address = network::SocketAddr::FromIPPort("127.0.0.1", server->GetPort());
        ASSERT_EQ(::connect(client, reinterpret_cast<const sockaddr*>(&address.ss), address.len), 0);
        timeval timeout{2, 0};
        ::setsockopt(client, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));
    }
    void TearDown() override
    {
        if (client >= 0) ::close(client);
        if (server) server->Stop();
        registry.Stop();
        for (const auto* path : {"live/auth", "live/second", "live/denied"})
            EXPECT_FALSE(media::MediaStreamRegistry::Instance().FindByName(path));
        server.reset(); loop.Stop();
        EXPECT_EQ(utils::EndpointManager::Instance().Size(), baseline);
    }
    std::string Request(const std::string& method, const std::string& uri = kUri,
                        const std::string& headers = {}, const std::string& body = {})
    {
        const auto request = method + " " + uri + " RTSP/1.0\r\nCSeq: " + std::to_string(++cseq) +
            "\r\n" + headers + "Content-Length: " + std::to_string(body.size()) + "\r\n\r\n" + body;
        std::size_t offset = 0;
        while (offset < request.size())
        {
            const auto sent = ::send(client, request.data() + offset, request.size() - offset, MSG_NOSIGNAL);
            if (sent <= 0) { ADD_FAILURE() << "RTSP send failed"; return {}; }
            offset += static_cast<std::size_t>(sent);
        }
        std::string response;
        char byte;
        while (response.find("\r\n\r\n") == std::string::npos && response.size() < 32768 && ::recv(client, &byte, 1, 0) == 1)
            response += byte;
        return response;
    }
    void Challenge()
    {
        const auto response = Request("ANNOUNCE", kUri, {}, kSdp);
        ASSERT_NE(response.find("401 Unauthorized"), std::string::npos) << response;
        nonce = ExtractNonce(response);
        ASSERT_FALSE(nonce.empty());
    }
    std::string Auth(const std::string& method, const std::string& uri = kUri,
                     const std::string& user = "alice", bool legacy = false, const std::string& password = "secret")
    {
        std::ostringstream count;
        count << std::hex << std::setw(8) << std::setfill('0') << ++nc;
        auto input = Md5(user + ":packetia:" + password) + ":" + nonce + ":";
        if (!legacy) input += count.str() + ":client:auth:";
        const auto response = Md5(input + Md5(method + ":" + uri));
        return "Authorization: Digest username=\"" + user + "\", realm=\"packetia\", nonce=\"" + nonce +
            "\", uri=\"" + uri + "\", response=\"" + response + "\"" +
            (legacy ? "" : ", qop=auth, nc=" + count.str() + ", cnonce=\"client\"") + "\r\n";
    }
    EventLoop loop;
    WorkerRegistry registry;
    std::shared_ptr<RtspServer> server;
    int client = -1, cseq = 0;
    uint32_t nc = 0;
    std::size_t baseline = 0;
    std::string nonce;
};

TEST_F(RtspAuthIntegration, UnauthenticatedCommandsCannotAllocateResources)
{
    EXPECT_NE(Request("OPTIONS", "*").find("200 OK"), std::string::npos);
    for (const auto* method : {"ANNOUNCE", "SETUP", "RECORD", "TEARDOWN", "DESCRIBE", "PLAY"})
    {
        const auto response = Request(method, kUri, {}, method == std::string("ANNOUNCE") ? kSdp : "");
        EXPECT_NE(response.find("401 Unauthorized"), std::string::npos) << response;
        EXPECT_NE(response.find("WWW-Authenticate: Digest"), std::string::npos);
    }
    EXPECT_FALSE(media::MediaStreamRegistry::Instance().FindByName("live/auth"));
    EXPECT_EQ(utils::EndpointManager::Instance().Size(), baseline);
}

TEST_F(RtspAuthIntegration, AuthorizedPushChecksEveryCommandAndPreservesSessionOnRejection)
{
    Challenge();
    ASSERT_NE(Request("ANNOUNCE", kUri, Auth("ANNOUNCE"), kSdp).find("200 OK"), std::string::npos);
    const auto session = media::MediaStreamRegistry::Instance().FindByName("live/auth");
    ASSERT_TRUE(session);
    const auto setup_uri = kUri + "/trackID=0";
    auto headers = Auth("SETUP", setup_uri) + "Transport: RTP/AVP/TCP;unicast;interleaved=0-1\r\n";
    ASSERT_NE(Request("SETUP", setup_uri, headers).find("200 OK"), std::string::npos);
    EXPECT_EQ(utils::EndpointManager::Instance().Size(), baseline + 1);
    const auto session_header = "Session: " + std::to_string(session->GetId()) + "\r\n";
    const auto record_auth = Auth("RECORD");
    EXPECT_NE(Request("RECORD", kUri, record_auth + session_header).find("200 OK"), std::string::npos);
    EXPECT_NE(Request("RECORD", kUri, record_auth + session_header).find("401 Unauthorized"), std::string::npos);
    EXPECT_NE(Request("TEARDOWN", kUri, session_header).find("401 Unauthorized"), std::string::npos);
    EXPECT_NE(Request("TEARDOWN", "rtsp://127.0.0.1/live/second",
        Auth("TEARDOWN", "rtsp://127.0.0.1/live/second") + session_header).find("403 Forbidden"), std::string::npos);
    EXPECT_NE(Request("TEARDOWN", kUri, Auth("TEARDOWN", kUri, "viewer") + session_header).find("403 Forbidden"), std::string::npos);
    EXPECT_EQ(utils::EndpointManager::Instance().Size(), baseline + 1);
    EXPECT_NE(Request("TEARDOWN", kUri, Auth("TEARDOWN") + session_header).find("200 OK"), std::string::npos);
}

TEST_F(RtspAuthIntegration, InvalidDigestAndForbiddenStreamsLeaveNoPartialResources)
{
    Challenge();
    EXPECT_NE(Request("ANNOUNCE", kUri, Auth("ANNOUNCE", kUri, "alice", false, "bad"), kSdp)
        .find("401 Unauthorized"), std::string::npos);
    const std::string denied = "rtsp://127.0.0.1/live/denied";
    EXPECT_NE(Request("ANNOUNCE", denied, Auth("ANNOUNCE", denied), kSdp).find("403 Forbidden"), std::string::npos);
    EXPECT_NE(Request("ANNOUNCE", denied, Auth("ANNOUNCE"), kSdp).find("401 Unauthorized"), std::string::npos);
    const auto duplicate = Auth("ANNOUNCE");
    EXPECT_NE(Request("ANNOUNCE", kUri, duplicate + duplicate, kSdp).find("400 Bad Request"), std::string::npos);
    EXPECT_FALSE(media::MediaStreamRegistry::Instance().FindByName("live/auth"));
    EXPECT_FALSE(media::MediaStreamRegistry::Instance().FindByName("live/denied"));
    EXPECT_EQ(utils::EndpointManager::Instance().Size(), baseline);
}

TEST_F(RtspAuthIntegration, PlayPermissionCannotAuthorizePushOrSetup)
{
    Challenge();
    EXPECT_NE(Request("ANNOUNCE", kUri, Auth("ANNOUNCE", kUri, "viewer"), kSdp).find("403 Forbidden"), std::string::npos);
    const auto uri = kUri + "/trackID=0";
    EXPECT_NE(Request("SETUP", uri, Auth("SETUP", uri, "viewer") +
        "Transport: RTP/AVP/TCP;unicast;interleaved=0-1\r\n").find("403 Forbidden"), std::string::npos);
    // 通过播放授权仍不代表播放业务已实现。
    EXPECT_NE(Request("DESCRIBE", kUri, Auth("DESCRIBE", kUri, "viewer")).find("501 Not Implemented"), std::string::npos);
    EXPECT_FALSE(media::MediaStreamRegistry::Instance().FindByName("live/auth"));
}

TEST_F(RtspAuthIntegration, LegacyClientRefreshesSingleUseNonceBeforeSetup)
{
    Challenge();
    ASSERT_NE(Request("ANNOUNCE", kUri, Auth("ANNOUNCE", kUri, "alice", true), kSdp).find("200 OK"), std::string::npos);
    const auto uri = kUri + "/trackID=0";
    const std::string transport = "Transport: RTP/AVP/TCP;unicast;interleaved=0-1\r\n";
    const auto stale = Request("SETUP", uri, Auth("SETUP", uri, "alice", true) + transport);
    ASSERT_NE(stale.find("401 Unauthorized"), std::string::npos);
    ASSERT_NE(stale.find("stale=true"), std::string::npos);
    nonce = ExtractNonce(stale);
    EXPECT_NE(Request("SETUP", uri, Auth("SETUP", uri, "alice", true) + transport).find("200 OK"), std::string::npos);
}
}
