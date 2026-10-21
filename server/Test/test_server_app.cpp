#include <gtest/gtest.h>

#include "ServerApp.h"
#include "TurnServer.h"

#include <cstdlib>
#include <optional>
#include <poll.h>
#include <stdexcept>
#include <unistd.h>

namespace
{
class Environment
{
public:
    explicit Environment(const char* name) : name_(name)
    {
        if (const auto* value = std::getenv(name)) original_ = value;
    }
    ~Environment() { Set(original_ ? original_->c_str() : nullptr); }
    void Set(const char* value)
    {
        if (value) setenv(name_, value, 1);
        else unsetenv(name_);
    }
private:
    const char* name_;
    std::optional<std::string> original_;
};

class Socket
{
public:
    Socket(int type, const std::string& ip = "127.0.0.1", uint16_t port = 0)
        : address(network::SocketAddr::FromIPPort(ip, port))
    {
        fd = socket(address.ss.ss_family, type, 0);
        if (fd < 0) throw std::runtime_error("socket creation failed");
        if (bind(fd, reinterpret_cast<const sockaddr*>(&address.ss), address.len) != 0 ||
            getsockname(fd, reinterpret_cast<sockaddr*>(&address.ss), &address.len) != 0)
        {
            close(fd);
            throw std::runtime_error("socket bind failed");
        }
    }
    ~Socket() { close(fd); }
    Socket(const Socket&) = delete;
    Socket& operator=(const Socket&) = delete;
    bool Send(const network::SocketAddr& target, const std::vector<uint8_t>& data)
    {
        return sendto(fd, data.data(), data.size(), 0, reinterpret_cast<const sockaddr*>(&target.ss), target.len)
            == static_cast<ssize_t>(data.size());
    }
    std::vector<uint8_t> Receive(int timeout_ms = 1000)
    {
        pollfd event{fd, POLLIN, 0};
        if (poll(&event, 1, timeout_ms) != 1) return {};
        std::vector<uint8_t> data(65536);
        const auto size = recv(fd, data.data(), data.size(), 0);
        if (size < 0) return {};
        data.resize(static_cast<size_t>(size));
        return data;
    }
    int fd = -1;
    network::SocketAddr address;
};

protocol::StunAttribute Text(protocol::AttrType type, const std::string& value)
{
    return {static_cast<uint16_t>(type), {value.begin(), value.end()}};
}

class ServerAppTest : public testing::Test
{
protected:
    void SetUp() override
    {
        username.Set("server-app-test");
        password.Set("test-secret");
        config.listen_ip = "127.0.0.1";
        config.recording_enabled = config.ai_enabled = config.conference_mix_enabled = false;
        config.rtsp_port = Reserve(SOCK_STREAM);
        config.sip_port = Reserve(SOCK_STREAM);
        config.rtmp_port = Reserve(SOCK_STREAM);
        config.websocket_port = Reserve(SOCK_STREAM);
        config.udp_port = Reserve(SOCK_DGRAM);
        config.turn.enabled = true;
        config.turn.listen_ip = "::";
        config.turn.dual_stack = true;
        config.turn.listen_port = Reserve(SOCK_DGRAM, "::");
        config.turn.relay_port_min = config.turn.relay_port_max = 0;
        config.turn.relay_bind_ip_v6 = config.turn.advertised_ip_v6 = "::1";
        config.turn.local_test = true;
#ifdef PACKETIA_WITH_LIBWEBSOCKETS
        config.webrtc.enabled = true;
        config.webrtc.public_ip = "127.0.0.1";
        config.webrtc.api_keys.emplace("server-app", "server-app-secret-at-least-32-bytes");
#endif
    }
    uint16_t Reserve(int type, const std::string& ip = "127.0.0.1")
    {
        reservations.push_back(std::make_unique<Socket>(type, ip));
        return reservations.back()->address.Port();
    }
    Environment username{"PACKETIA_TURN_USER"}, password{"PACKETIA_TURN_PASSWORD"};
    server::ServerConfig config;
    std::vector<std::unique_ptr<Socket>> reservations;
};

TEST_F(ServerAppTest, TranscodeTasksCanStartStopAndFollowServiceToggle)
{
    config.turn.enabled = false;
    reservations.clear();
    server::ServerApp app(config);
    service::transcode::TranscodeTaskConfig task;
    task.task_id = "app-transcode";
    task.input.endpoint_id = 10;
    task.input.track_id = 42;
    task.output.endpoint_id = 20;
    task.output.session_id = "room";
    task.output.stream_id = "camera";
    EXPECT_FALSE(app.StartTranscode(task));
    EXPECT_FALSE(app.TranscodeLastError().empty());
    ASSERT_TRUE(app.Start());
    ASSERT_TRUE(app.ServiceEnabled(service::ServiceType::Transcode));
    ASSERT_TRUE(app.TranscodedFrameRouter());
    EXPECT_NE(app.TranscodedFrameRouter(), app.MixedFrameRouter());
    ASSERT_TRUE(app.StartTranscode(task));
    auto status = app.TranscodeStatus(task.task_id);
    ASSERT_TRUE(status);
    EXPECT_EQ(status->state, service::transcode::TranscodeTaskState::Running);
    EXPECT_TRUE(status->waiting_for_idr);
    ASSERT_TRUE(app.SetServiceEnabled(service::ServiceType::Transcode, false));
    EXPECT_FALSE(app.TranscodeStatus(task.task_id));
    EXPECT_FALSE(app.StartTranscode(task));
    ASSERT_TRUE(app.SetServiceEnabled(service::ServiceType::Transcode, true));
    ASSERT_TRUE(app.StartTranscode(task));
    service::transcode::TranscodeTaskStatus final;
    ASSERT_TRUE(app.StopTranscode(task.task_id, &final));
    EXPECT_EQ(final.state, service::transcode::TranscodeTaskState::Stopped);
    EXPECT_EQ(final.stats.published, 0u);
    ASSERT_TRUE(app.StartTranscode(task));
    app.Stop();
    EXPECT_FALSE(app.TranscodeStatus(task.task_id));
}

TEST_F(ServerAppTest, SingleAppStartsTurnAndWebRtcRelaysAndReleasesResources)
{
    using namespace protocol;
    config.turn.unauthorized_ratelimit = true;
    config.turn.unauthorized_ratelimit_rps = 1;
    reservations.clear();
    server::ServerApp app(config);
    ASSERT_TRUE(app.Start());
    ASSERT_TRUE(app.Start());
#ifdef PACKETIA_WITH_LIBWEBSOCKETS
    Socket ws(SOCK_STREAM);
    const auto ws_target = network::SocketAddr::FromIPPort("127.0.0.1", config.websocket_port);
    ASSERT_EQ(connect(ws.fd, reinterpret_cast<const sockaddr*>(&ws_target.ss), ws_target.len), 0);
    const std::string upgrade = "GET / HTTP/1.1\r\nHost: localhost\r\nUpgrade: websocket\r\n"
        "Connection: Upgrade\r\nSec-WebSocket-Version: 13\r\nSec-WebSocket-Protocol: packetia\r\n"
        "Sec-WebSocket-Key: dGhlIHNhbXBsZSBub25jZQ==\r\n\r\n";
    ASSERT_EQ(send(ws.fd, upgrade.data(), upgrade.size(), 0), static_cast<ssize_t>(upgrade.size()));
    const auto upgraded = ws.Receive();
    ASSERT_NE(std::string(upgraded.begin(), upgraded.end()).find("101"), std::string::npos);
#endif
    Socket client(SOCK_DGRAM), peer(SOCK_DGRAM);
    const auto control = network::SocketAddr::FromIPPort("127.0.0.1", config.turn.listen_port);
    std::array<uint8_t, 12> id{};
    auto request = StunCodec::BuildBindingRequest(id);
    ASSERT_TRUE(client.Send(control, request));
    auto response = client.Receive();
    StunMessageInfo message;
    ASSERT_TRUE(TurnCodec::ParseStunDatagram(response.data(), response.size(), message));
    ASSERT_TRUE(message.IsBindingResponse());
    EXPECT_EQ(message.txid, id);
    XorMappedAddress mapped;
    ASSERT_TRUE(StunCodec::DecodeXorMappedAddress(message, mapped));
    EXPECT_FALSE(mapped.is_ipv6);
    EXPECT_EQ(mapped.port, client.address.Port());
    ASSERT_TRUE(StunCodec::BuildMessage(StunMethod::Allocate, StunClass::Request, id,
        {TurnCodec::RequestedTransportAttribute(17)}, request));
    ASSERT_TRUE(client.Send(control, request));
    response = client.Receive();
    ASSERT_TRUE(TurnCodec::ParseStunDatagram(response.data(), response.size(), message));
    StunErrorCode error;
    ASSERT_TRUE(StunCodec::DecodeErrorCode(message, error));
    ASSERT_EQ(error.code, 401);
    // ServerApp must pass the configured limiter through to TURN. Cached
    // challenges are suppressed, while subsequent authenticated relay works.
    ASSERT_TRUE(client.Send(control, request));
    EXPECT_TRUE(client.Receive(50).empty());
    const auto* nonce = message.FindAttr(static_cast<uint16_t>(AttrType::NONCE));
    ASSERT_NE(nonce, nullptr);
    const std::vector<StunAttribute> credentials = {Text(AttrType::USERNAME, "server-app-test"),
        Text(AttrType::REALM, config.turn.realm), Text(AttrType::NONCE, std::string(message.AttrValue(*nonce)))};
    std::string key;
    ASSERT_TRUE(TurnAuth::DerivePreparedLegacyKey("server-app-test", config.turn.realm, "test-secret", key));
    const auto exchange = [&](StunMethod method, const std::vector<StunAttribute>& extra) {
        auto attributes = credentials;
        attributes.insert(attributes.end(), extra.begin(), extra.end());
        ++id.back();
        EXPECT_TRUE(StunCodec::BuildMessage(method, StunClass::Request, id, attributes, request, key, true));
        EXPECT_TRUE(client.Send(control, request));
        return client.Receive();
    };
    response = exchange(StunMethod::Allocate, {TurnCodec::RequestedTransportAttribute(17)});
    ASSERT_TRUE(TurnCodec::ParseStunDatagram(response.data(), response.size(), message));
    ASSERT_EQ(message.klass, StunClass::SuccessResponse);
    XorMappedAddress relayed;
    ASSERT_TRUE(StunCodec::DecodeXorAddress(message, AttrType::XOR_RELAYED_ADDRESS, relayed));
    const auto relay = network::SocketAddr::FromIPPort("127.0.0.1", relayed.port);
    IpEndpoint target;
    target.port = peer.address.Port();
    const auto ip = peer.address.IPv4Bytes();
    std::copy(ip.begin(), ip.end(), target.ip.begin());
    StunAttribute peer_attribute, channel;
    auto channel_id = id;
    ++channel_id.back();
    ASSERT_TRUE(TurnCodec::XorAddressAttribute(AttrType::XOR_PEER_ADDRESS, target, channel_id, peer_attribute));
    ASSERT_TRUE(TurnCodec::ChannelNumberAttribute(0x4000, channel));
    response = exchange(StunMethod::ChannelBind, {peer_attribute, channel});
    ASSERT_TRUE(TurnCodec::ParseStunDatagram(response.data(), response.size(), message));
    ASSERT_EQ(message.klass, StunClass::SuccessResponse);
    ASSERT_TRUE(TurnCodec::BuildChannelData(0x4000, "through-one-app", request));
    ASSERT_TRUE(client.Send(control, request));
    EXPECT_EQ(peer.Receive(), (std::vector<uint8_t>{'t','h','r','o','u','g','h','-','o','n','e','-','a','p','p'}));
    ASSERT_TRUE(peer.Send(relay, {'o', 'k'}));
    response = client.Receive();
    TurnChannelDataView received;
    ASSERT_TRUE(TurnCodec::ParseChannelDataDatagram(response.data(), response.size(), received));
    EXPECT_EQ(received.data, "ok");
    app.Stop();
    app.Stop();
    { Socket released_control(SOCK_DGRAM, "::", config.turn.listen_port); }
    { Socket released_relay(SOCK_DGRAM, "127.0.0.1", relay.Port()); }
    ASSERT_TRUE(app.Start());
    app.Stop();
}

TEST_F(ServerAppTest, LaterStartupFailureRollsBackTurnListener)
{
    reservations.clear();
    Socket occupied(SOCK_STREAM, "127.0.0.1", config.rtsp_port);
    ASSERT_EQ(listen(occupied.fd, 1), 0);
    server::ServerApp app(config);
    EXPECT_FALSE(app.Start());
    for (const auto& thread : app.ThreadStatus().threads)
        EXPECT_EQ(thread.options.model, utils::ThreadModel::Logging);
    Socket released_control(SOCK_DGRAM, "::", config.turn.listen_port);
}

TEST_F(ServerAppTest, RuntimeReportsConfiguredThreadsAndRemovesThemAcrossRestart)
{
    config.io_threads = 2;
    config.workers.media_threads = 2;
    config.workers.endpoint_threads = 1;
    config.workers.transcode_threads = 1;
    reservations.clear();
    server::ServerApp app(config);
    for (int cycle = 0; cycle != 2; ++cycle)
    {
        ASSERT_TRUE(app.Start());
        const auto status = app.ThreadStatus();
        ASSERT_EQ(status.io.size(), 2u);
        for (const auto& io : status.io) EXPECT_TRUE(io.started);
        ASSERT_EQ(status.pools.size(), 3u);
        size_t io_threads = 0, workers = 0;
        for (const auto& thread : status.threads)
        {
            if (thread.options.model == utils::ThreadModel::Io) ++io_threads;
            if (thread.options.model == utils::ThreadModel::Worker) ++workers;
        }
        EXPECT_EQ(io_threads, 2u);
        EXPECT_EQ(workers, 4u);
        app.Stop();
        const auto stopped = app.ThreadStatus();
        EXPECT_TRUE(stopped.io.empty());
        EXPECT_TRUE(stopped.pools.empty());
        for (const auto& thread : stopped.threads)
            EXPECT_EQ(thread.options.model, utils::ThreadModel::Logging);
    }
}

TEST_F(ServerAppTest, AccountStorageFailureRollsBackAndAllowsExistingServicesToStart)
{
    config.account.enabled = true;
    config.account.database_path = "/dev/null/accounts.db";
    config.http.enabled = true;
    config.http.listen_port = Reserve(SOCK_STREAM);
    reservations.clear();
    server::ServerApp incomplete(config);
    EXPECT_FALSE(incomplete.Start());
    EXPECT_FALSE(incomplete.Start());
    incomplete.Stop();
    incomplete.Stop();
    // No account API may listen when its storage failed to initialize.
    EXPECT_NO_THROW({ Socket unused_api(SOCK_STREAM, "127.0.0.1", config.http.listen_port); });

    config.account.enabled = config.http.enabled = false;
    server::ServerApp existing(config);
    ASSERT_TRUE(existing.Start());
    existing.Stop();
}

TEST_F(ServerAppTest, AccountApiStartsWithRealPasswordAndStorageProviders)
{
    config.account.enabled = true;
    config.account.database_path = ":memory:";
    config.http.enabled = true;
    config.http.listen_port = Reserve(SOCK_STREAM);
    reservations.clear();
    server::ServerApp app(config);
    ASSERT_TRUE(app.Start());
    EXPECT_THROW({ Socket occupied(SOCK_STREAM, "127.0.0.1", config.http.listen_port); }, std::runtime_error);
    app.Stop();
    EXPECT_NO_THROW({ Socket available(SOCK_STREAM, "127.0.0.1", config.http.listen_port); });
}

TEST_F(ServerAppTest, MissingTurnCredentialsFailStartupAndDisabledTurnDoesNotBind)
{
    reservations.clear();
    password.Set(nullptr);
    {
        server::ServerApp app(config);
        EXPECT_FALSE(app.Start());
    }
    Socket unused_turn(SOCK_DGRAM, "::", config.turn.listen_port);
    config.turn.enabled = false;
    server::ServerApp app(config);
    ASSERT_TRUE(app.Start());
    app.Stop();
}
} // namespace
