#include <gtest/gtest.h>

#include "EventLoop.h"
#include "IceAgent.h"
#include "TurnServer.h"
#include "utils.h"

#include <atomic>
#include <poll.h>
#include <unistd.h>

namespace protocol
{
namespace
{
class UdpPeer
{
public:
    explicit UdpPeer(const std::string& ip = "127.0.0.1")
    {
        Open(ip);
    }
    void Open(const std::string& ip)
    {
        if (fd >= 0) close(fd);
        address = network::SocketAddr::FromIPPort(ip, 0);
        fd = socket(address.ss.ss_family, SOCK_DGRAM, 0);
        if (fd >= 0 && bind(fd, reinterpret_cast<sockaddr*>(&address.ss), address.len) == 0)
            getsockname(fd, reinterpret_cast<sockaddr*>(&address.ss), &address.len);
    }
    ~UdpPeer() { if (fd >= 0) close(fd); }
    UdpPeer(const UdpPeer&) = delete;
    UdpPeer& operator=(const UdpPeer&) = delete;
    bool Send(const network::SocketAddr& target, const std::vector<uint8_t>& bytes)
    {
        return sendto(fd, bytes.data(), bytes.size(), 0, reinterpret_cast<const sockaddr*>(&target.ss), target.len)
            == static_cast<ssize_t>(bytes.size());
    }
    bool Receive(std::vector<uint8_t>& bytes, network::SocketAddr* source = nullptr, int timeout = 1000)
    {
        pollfd event{fd, POLLIN, 0};
        if (poll(&event, 1, timeout) != 1) return false;
        network::SocketAddr from;
        from.len = sizeof(from.ss);
        bytes.resize(65536);
        const auto n = recvfrom(fd, bytes.data(), bytes.size(), 0, reinterpret_cast<sockaddr*>(&from.ss), &from.len);
        if (n < 0) return false;
        bytes.resize(static_cast<size_t>(n));
        if (source) *source = from;
        return true;
    }
    int fd = -1;
    network::SocketAddr address;
};

StunAttribute Text(AttrType type, std::string_view text)
{
    return {static_cast<uint16_t>(type), {text.begin(), text.end()}};
}

class TurnServerTest : public testing::Test
{
protected:
    void SetUp() override
    {
        ASSERT_GT(client.address.Port(), 0);
        ASSERT_GT(peer.address.Port(), 0);
        ASSERT_TRUE(loop.Start());
        scheduler = loop.GetTaskScheduler();
        options.listen_port = 0;
        options.relay_port_min = options.relay_port_max = 0;
        options.realm = "example.org";
        options.clock = [this] { return now.load(); };
        options.allow_peer = [](const network::SocketAddr& source) {
            return source.IPv4Bytes() == std::string("\x7f\x00\x00\x01", 4);
        };
        StartServer();
    }
    void TearDown() override
    {
        if (server) server->Stop();
        server.reset();
        loop.Stop();
    }
    void StartServer()
    {
        if (server) server->Stop();
        server = std::make_shared<TurnServer>(scheduler, options,
            [](std::string_view username, std::string& password) {
                if (username != "alice" && username != "bob") return false;
                password = "secret";
                return true;
            });
        ASSERT_TRUE(server->Start());
        control = server->LocalAddress();
        ASSERT_GT(control.Port(), 0);
    }
    std::array<uint8_t, 12> Id()
    {
        std::array<uint8_t, 12> id{};
        utils::Utils::WriteUint32BE(id.data(), ++transaction);
        return id;
    }
    std::vector<uint8_t> Request(StunMethod method, const std::vector<StunAttribute>& extra,
                                 const std::array<uint8_t, 12>& id, std::string_view username = "alice")
    {
        std::string key;
        EXPECT_TRUE(TurnAuth::DerivePreparedLegacyKey(username, options.realm, "secret", key));
        auto attrs = std::vector<StunAttribute>{Text(AttrType::USERNAME, username),
            Text(AttrType::REALM, options.realm), Text(AttrType::NONCE, nonce)};
        attrs.insert(attrs.end(), extra.begin(), extra.end());
        std::vector<uint8_t> bytes;
        EXPECT_TRUE(StunCodec::BuildMessage(method, StunClass::Request, id, attrs, bytes, key, true));
        return bytes;
    }
    std::vector<uint8_t> Exchange(const std::vector<uint8_t>& request, UdpPeer* socket = nullptr)
    {
        auto& source = socket ? *socket : client;
        EXPECT_TRUE(source.Send(control, request));
        std::vector<uint8_t> response;
        EXPECT_TRUE(source.Receive(response));
        return response;
    }
    void ExpectCode(const std::vector<uint8_t>& bytes, uint16_t code, std::string_view username = "alice")
    {
        StunMessageInfo msg;
        ASSERT_TRUE(TurnCodec::ParseStunDatagram(bytes.data(), bytes.size(), msg));
        EXPECT_TRUE(StunCodec::VerifyFingerprint(msg));
        if (code == 0) EXPECT_EQ(msg.klass, StunClass::SuccessResponse);
        else
        {
            StunErrorCode error;
            ASSERT_TRUE(StunCodec::DecodeErrorCode(msg, error));
            EXPECT_EQ(error.code, code);
        }
        if (code != 401 && code != 438)
        {
            std::string key;
            ASSERT_TRUE(TurnAuth::DerivePreparedLegacyKey(username, options.realm, "secret", key));
            EXPECT_TRUE(StunCodec::VerifyMessageIntegrity(msg, key));
        }
    }
    void ExpectBinding(const std::vector<uint8_t>& bytes, const UdpPeer& source,
                       const std::array<uint8_t, 12>& id)
    {
        StunMessageInfo msg;
        ASSERT_TRUE(TurnCodec::ParseStunDatagram(bytes.data(), bytes.size(), msg));
        EXPECT_TRUE(msg.IsBindingResponse());
        EXPECT_EQ(msg.txid, id);
        EXPECT_TRUE(StunCodec::VerifyFingerprint(msg));
        EXPECT_FALSE(msg.HasAttr(static_cast<uint16_t>(AttrType::MESSAGE_INTEGRITY)));
        EXPECT_FALSE(msg.HasAttr(static_cast<uint16_t>(AttrType::REALM)));
        EXPECT_FALSE(msg.HasAttr(static_cast<uint16_t>(AttrType::NONCE)));
        EXPECT_FALSE(msg.HasAttr(static_cast<uint16_t>(AttrType::XOR_RELAYED_ADDRESS)));
        XorMappedAddress mapped;
        ASSERT_TRUE(StunCodec::DecodeXorMappedAddress(msg, mapped));
        EXPECT_EQ(mapped.is_ipv6, source.address.IsV6());
        EXPECT_EQ(mapped.port, source.address.Port());
        const auto ip = source.address.IsV6() ? source.address.IPv6Bytes() : source.address.IPv4Bytes();
        EXPECT_EQ(std::string(reinterpret_cast<const char*>(mapped.ip.data()), ip.size()), ip);
    }
    void Challenge(UdpPeer* socket = nullptr)
    {
        std::vector<uint8_t> request;
        ASSERT_TRUE(StunCodec::BuildMessage(StunMethod::Allocate, StunClass::Request, Id(),
            {TurnCodec::RequestedTransportAttribute(17)}, request));
        const auto response = Exchange(request, socket);
        ExpectCode(response, 401);
        StunMessageInfo msg;
        ASSERT_TRUE(TurnCodec::ParseStunDatagram(response.data(), response.size(), msg));
        const auto* attr = msg.FindAttr(static_cast<uint16_t>(AttrType::NONCE));
        ASSERT_NE(attr, nullptr);
        nonce = std::string(msg.AttrValue(*attr));
    }
    std::vector<uint8_t> Allocate(uint8_t family = 0)
    {
        Challenge();
        std::vector<StunAttribute> attributes = {TurnCodec::RequestedTransportAttribute(17)};
        if (family != 0)
            attributes.push_back({static_cast<uint16_t>(AttrType::REQUESTED_ADDRESS_FAMILY), {family, 0, 0, 0}});
        const auto request = Request(StunMethod::Allocate, attributes, Id());
        const auto response = Exchange(request);
        ExpectCode(response, 0);
        StunMessageInfo msg;
        EXPECT_TRUE(TurnCodec::ParseStunDatagram(response.data(), response.size(), msg));
        XorMappedAddress addr;
        EXPECT_TRUE(StunCodec::DecodeXorAddress(msg, AttrType::XOR_RELAYED_ADDRESS, addr));
        EXPECT_EQ(addr.is_ipv6, family == 2);
        relay = network::SocketAddr::FromIPPort(addr.is_ipv6 ? "::1" : "127.0.0.1", addr.port);
        EXPECT_GT(relay.Port(), 0);
        EXPECT_TRUE(StunCodec::DecodeXorMappedAddress(msg, addr));
        EXPECT_EQ(addr.is_ipv6, client.address.IsV6());
        EXPECT_EQ(addr.port, client.address.Port());
        const auto client_ip = client.address.IsV6() ? client.address.IPv6Bytes() : client.address.IPv4Bytes();
        EXPECT_EQ(std::string(reinterpret_cast<const char*>(addr.ip.data()), client_ip.size()), client_ip);
        return request;
    }
    StunAttribute PeerAttribute(const network::SocketAddr& source, const std::array<uint8_t, 12>& id)
    {
        IpEndpoint ep;
        ep.port = source.Port();
        ep.family = source.IsV6() ? IpFamily::IPv6 : IpFamily::IPv4;
        const auto bytes = source.IsV6() ? source.IPv6Bytes() : source.IPv4Bytes();
        std::copy(bytes.begin(), bytes.end(), ep.ip.begin());
        StunAttribute attr;
        EXPECT_TRUE(TurnCodec::XorAddressAttribute(AttrType::XOR_PEER_ADDRESS, ep, id, attr));
        return attr;
    }
    std::vector<uint8_t> Permission(const network::SocketAddr& target)
    {
        const auto id = Id();
        return Exchange(Request(StunMethod::CreatePermission, {PeerAttribute(target, id)}, id));
    }
    std::vector<uint8_t> BindChannel(uint16_t number, const network::SocketAddr& target)
    {
        const auto id = Id();
        StunAttribute channel;
        EXPECT_TRUE(TurnCodec::ChannelNumberAttribute(number, channel));
        return Exchange(Request(StunMethod::ChannelBind, {channel, PeerAttribute(target, id)}, id));
    }
    std::vector<uint8_t> SendIndication(const network::SocketAddr& target, const std::vector<uint8_t>& data)
    {
        const auto id = Id();
        std::vector<uint8_t> packet;
        EXPECT_TRUE(StunCodec::BuildMessage(StunMethod::Send, StunClass::Indication, id,
            {PeerAttribute(target, id), {static_cast<uint16_t>(AttrType::DATA), data}}, packet));
        return packet;
    }

    EventLoop loop{1};
    std::shared_ptr<TaskScheduler> scheduler;
    TurnServerOptions options;
    std::shared_ptr<TurnServer> server;
    UdpPeer client, peer;
    network::SocketAddr control, relay;
    std::atomic<uint64_t> now{0};
    uint32_t transaction = 0;
    std::string nonce;
};
} // namespace

TEST_F(TurnServerTest, BindingDiscoversAddressWithoutAuthenticationOrAllocation)
{
    for (bool fingerprint : {false, true})
    {
        const auto id = Id();
        std::vector<uint8_t> request;
        ASSERT_TRUE(StunCodec::BuildMessage(StunMethod::Binding, StunClass::Request, id,
            {Text(AttrType::SOFTWARE, "basic-stun-client"), {0x800f, {1, 2, 3}}}, request, {}, fingerprint));
        const auto response = Exchange(request);
        ExpectBinding(response, client, id);
        EXPECT_EQ(Exchange(request), response);
        EXPECT_EQ(server->GetStats().sessions, 0u);
        EXPECT_EQ(server->GetStats().allocations, 0u);
    }
    // Binding is not an authorization grant for a subsequent Allocate.
    Challenge();
    EXPECT_EQ(server->GetStats().sessions, 1u);
    EXPECT_EQ(server->GetStats().allocations, 0u);
}

TEST_F(TurnServerTest, BindingWorksAtSessionQuotaAndDoesNotModifyAllocationOrCache)
{
    options.max_sessions = 1;
    StartServer();
    const auto allocate = Allocate();
    const auto cached = Exchange(allocate);
    StunMessageInfo allocation;
    ASSERT_TRUE(TurnCodec::ParseStunDatagram(allocate.data(), allocate.size(), allocation));
    const auto request = StunCodec::BuildBindingRequest(allocation.txid);
    ExpectBinding(Exchange(request), client, allocation.txid);
    UdpPeer other;
    ExpectBinding(Exchange(request, &other), other, allocation.txid);
    EXPECT_EQ(server->GetStats().sessions, 1u);
    EXPECT_EQ(server->GetStats().allocations, 1u);
    EXPECT_EQ(Exchange(allocate), cached);
    now = 599999;
    ExpectBinding(Exchange(request), client, allocation.txid);
    now = 600000;
    server->Tick();
    EXPECT_EQ(server->GetStats().allocations, 0u);
    EXPECT_EQ(server->GetStats().sessions, 0u);
}

TEST_F(TurnServerTest, BindingUnknownRequiredAttributesReturnUnsigned420)
{
    const auto id = Id();
    std::vector<uint8_t> request;
    ASSERT_TRUE(StunCodec::BuildMessage(StunMethod::Binding, StunClass::Request, id,
        {{0x0031, {}}, {0x0032, {1}}, {0x0031, {}}, {0x800f, {}}}, request, {}, true));
    const auto response = Exchange(request);
    StunMessageInfo msg;
    ASSERT_TRUE(TurnCodec::ParseStunDatagram(response.data(), response.size(), msg));
    EXPECT_TRUE(msg.IsBindingErrorResponse());
    EXPECT_EQ(msg.txid, id);
    EXPECT_TRUE(StunCodec::VerifyFingerprint(msg));
    StunErrorCode error;
    ASSERT_TRUE(StunCodec::DecodeErrorCode(msg, error));
    EXPECT_EQ(error.code, 420);
    const auto* unknown = msg.FindAttr(static_cast<uint16_t>(AttrType::UNKNOWN_ATTRIBUTES));
    ASSERT_NE(unknown, nullptr);
    EXPECT_EQ(msg.AttrValue(*unknown), std::string("\x00\x31\x00\x32", 4));
    EXPECT_FALSE(msg.HasAttr(static_cast<uint16_t>(AttrType::MESSAGE_INTEGRITY)));
    EXPECT_EQ(server->GetStats().sessions, 0u);
}

TEST_F(TurnServerTest, BindingIgnoresRecognizedAttributesWithoutLearningCredentials)
{
    const auto id = Id();
    std::vector<uint8_t> request;
    ASSERT_TRUE(StunCodec::BuildMessage(StunMethod::Binding, StunClass::Request, id,
        {Text(AttrType::USERNAME, "not-a-turn-user"), Text(AttrType::REALM, "not-the-realm"),
         TurnCodec::UInt32Attribute(AttrType::PRIORITY, 1234), TurnCodec::RequestedTransportAttribute(17)},
        request, "unverified-key", true));
    ExpectBinding(Exchange(request), client, id);
    EXPECT_EQ(server->GetStats().sessions, 0u);
    EXPECT_EQ(server->GetStats().allocations, 0u);
}

TEST_F(TurnServerTest, BindingDropsMalformedOversizedAndNonRequestDatagrams)
{
    std::vector<uint8_t> fingerprinted;
    ASSERT_TRUE(StunCodec::BuildMessage(StunMethod::Binding, StunClass::Request, Id(), {}, fingerprinted, {}, true));
    auto bad_fingerprint = fingerprinted;
    bad_fingerprint.back() ^= 1;
    auto duplicate_fingerprint = fingerprinted;
    duplicate_fingerprint.insert(duplicate_fingerprint.end(), fingerprinted.end() - 8, fingerprinted.end());
    utils::Utils::WriteUint16BE(duplicate_fingerprint.data() + 2, duplicate_fingerprint.size() - 20);
    utils::Utils::WriteUint32BE(duplicate_fingerprint.data() + duplicate_fingerprint.size() - 4,
        StunCodec::ComputeFingerprint(duplicate_fingerprint.data(), duplicate_fingerprint.size() - 8));
    const auto request = StunCodec::BuildBindingRequest(Id());
    auto truncated = request;
    truncated.pop_back();
    auto trailing = request;
    trailing.push_back(0);
    std::vector<uint8_t> malformed_attribute;
    ASSERT_TRUE(StunCodec::BuildMessage(StunMethod::Binding, StunClass::Request, Id(),
        {Text(AttrType::SOFTWARE, "")}, malformed_attribute));
    malformed_attribute[23] = 8;
    std::vector<uint8_t> oversized;
    ASSERT_TRUE(StunCodec::BuildMessage(StunMethod::Binding, StunClass::Request, Id(),
        {{static_cast<uint16_t>(AttrType::SOFTWARE), std::vector<uint8_t>(4096, 'x')}}, oversized));
    std::vector<std::vector<uint8_t>> packets = {bad_fingerprint, duplicate_fingerprint,
        truncated, trailing, malformed_attribute, oversized};
    for (auto klass : {StunClass::Indication, StunClass::SuccessResponse, StunClass::ErrorResponse})
    {
        std::vector<uint8_t> packet;
        ASSERT_TRUE(StunCodec::BuildMessage(StunMethod::Binding, klass, Id(), {}, packet));
        packets.push_back(std::move(packet));
    }
    for (const auto& packet : packets)
    {
        ASSERT_TRUE(client.Send(control, packet));
        std::vector<uint8_t> response;
        EXPECT_FALSE(client.Receive(response, nullptr, 30));
    }
    EXPECT_EQ(server->GetStats().sessions, 0u);
    EXPECT_EQ(server->GetStats().allocations, 0u);
}

TEST_F(TurnServerTest, AllocateRetransmissionRefreshAndDeletionUseRealSockets)
{
    const auto allocation_request = Allocate();
    const auto first = Exchange(allocation_request);
    EXPECT_EQ(Exchange(allocation_request), first);
    EXPECT_EQ(server->GetStats().allocations, 1u);
    ExpectCode(Exchange(Request(StunMethod::Allocate, {TurnCodec::RequestedTransportAttribute(17)}, Id())), 437);
    const auto refresh = Request(StunMethod::Refresh, {TurnCodec::UInt32Attribute(AttrType::LIFETIME, 0)}, Id());
    const auto deleted = Exchange(refresh);
    ExpectCode(deleted, 0);
    EXPECT_EQ(Exchange(refresh), deleted);
    EXPECT_EQ(server->GetStats().allocations, 0u);
    const int fd = socket(AF_INET, SOCK_DGRAM, 0);
    ASSERT_GE(fd, 0);
    EXPECT_EQ(bind(fd, reinterpret_cast<sockaddr*>(&relay.ss), relay.len), 0);
    close(fd);
}

TEST_F(TurnServerTest, SendAndDataIndicationsRelayBinaryDataInBothDirections)
{
    Allocate();
    const std::vector<uint8_t> payload = {0, 1, 0x80, 0xFF};
    ASSERT_TRUE(client.Send(control, SendIndication(peer.address, payload)));
    std::vector<uint8_t> data;
    EXPECT_FALSE(peer.Receive(data, nullptr, 50));
    ExpectCode(Permission(peer.address), 0);
    ASSERT_TRUE(client.Send(control, SendIndication(peer.address, payload)));
    network::SocketAddr source;
    ASSERT_TRUE(peer.Receive(data, &source));
    EXPECT_EQ(data, payload);
    EXPECT_TRUE(source == relay);
    ASSERT_TRUE(peer.Send(relay, payload));
    ASSERT_TRUE(client.Receive(data));
    StunMessageInfo msg;
    ASSERT_TRUE(TurnCodec::ParseStunDatagram(data.data(), data.size(), msg));
    EXPECT_EQ(msg.method, StunMethod::Data);
    EXPECT_EQ(msg.klass, StunClass::Indication);
    const auto* attr = msg.FindAttr(static_cast<uint16_t>(AttrType::DATA));
    ASSERT_NE(attr, nullptr);
    EXPECT_EQ(msg.AttrValue(*attr), std::string(reinterpret_cast<const char*>(payload.data()), payload.size()));
    XorMappedAddress address;
    ASSERT_TRUE(StunCodec::DecodeXorAddress(msg, AttrType::XOR_PEER_ADDRESS, address));
    EXPECT_EQ(address.port, peer.address.Port());
    EXPECT_EQ(server->GetStats().client_to_peer, 1u);
    EXPECT_EQ(server->GetStats().peer_to_client, 1u);
}

TEST_F(TurnServerTest, ChannelDataRelaysBinaryAndEmptyDatagrams)
{
    Allocate();
    ExpectCode(BindChannel(0x4001, peer.address), 0);
    for (const auto& payload : {std::vector<uint8_t>{1, 0, 3}, std::vector<uint8_t>{}})
    {
        std::vector<uint8_t> packet, received;
        ASSERT_TRUE(TurnCodec::BuildChannelData(0x4001,
            {reinterpret_cast<const char*>(payload.data()), payload.size()}, packet));
        ASSERT_TRUE(client.Send(control, packet));
        ASSERT_TRUE(peer.Receive(received));
        EXPECT_EQ(received, payload);
        ASSERT_TRUE(peer.Send(relay, payload));
        ASSERT_TRUE(client.Receive(received));
        TurnChannelDataView view;
        ASSERT_TRUE(TurnCodec::ParseChannelDataDatagram(received.data(), received.size(), view));
        EXPECT_EQ(view.channel, 0x4001);
        EXPECT_EQ(view.data, std::string_view(reinterpret_cast<const char*>(payload.data()), payload.size()));
    }
}

TEST_F(TurnServerTest, PermissionsMatchIpAndChannelsMatchFullPeerAddress)
{
    Allocate();
    UdpPeer second;
    auto permission_address = peer.address;
    reinterpret_cast<sockaddr_in*>(&permission_address.ss)->sin_port = 0;
    ExpectCode(Permission(permission_address), 0);
    ASSERT_TRUE(client.Send(control, SendIndication(second.address, {1, 2})));
    std::vector<uint8_t> received;
    ASSERT_TRUE(second.Receive(received));
    ExpectCode(BindChannel(0x4000, peer.address), 0);
    ExpectCode(BindChannel(0x4000, second.address), 400);
    ExpectCode(BindChannel(0x4001, peer.address), 400);
    EXPECT_EQ(server->GetStats().allocations, 1u);
}

TEST_F(TurnServerTest, PermissionExpiryDoesNotRenewFromDataAndChannelExpiresSeparately)
{
    Allocate();
    ExpectCode(BindChannel(0x4000, peer.address), 0);
    now = 299999;
    ExpectCode(Exchange(Request(StunMethod::Refresh, {TurnCodec::UInt32Attribute(AttrType::LIFETIME, 600)}, Id())), 0);
    ASSERT_TRUE(peer.Send(relay, {1}));
    std::vector<uint8_t> data;
    ASSERT_TRUE(client.Receive(data));
    now = 300000;
    server->Tick();
    ASSERT_TRUE(peer.Send(relay, {2}));
    EXPECT_FALSE(client.Receive(data, nullptr, 50));
    ASSERT_TRUE(client.Send(control, SendIndication(peer.address, {3})));
    EXPECT_FALSE(peer.Receive(data, nullptr, 50));
    // RFC 8656 12.6 allows outbound data on a live channel independently
    // of the inbound peer permission, without renewing either lifetime.
    std::vector<uint8_t> channel;
    ASSERT_TRUE(TurnCodec::BuildChannelData(0x4000, "channel", channel));
    ASSERT_TRUE(client.Send(control, channel));
    ASSERT_TRUE(peer.Receive(data));
    now = 599999;
    ExpectCode(Permission(peer.address), 0);
    now = 600000;
    server->Tick();
    ASSERT_TRUE(client.Send(control, channel));
    EXPECT_FALSE(peer.Receive(data, nullptr, 50));
    ASSERT_TRUE(peer.Send(relay, {4}));
    ASSERT_TRUE(client.Receive(data));
    StunMessageInfo msg;
    ASSERT_TRUE(TurnCodec::ParseStunDatagram(data.data(), data.size(), msg));
    EXPECT_EQ(msg.method, StunMethod::Data);
}

TEST_F(TurnServerTest, AllocationExpiryAndUnauthenticatedSessionCleanupAreScheduled)
{
    Allocate();
    now = 599999;
    EXPECT_EQ(server->GetStats().allocations, 1u);
    now = 600000;
    server->Tick();
    EXPECT_EQ(server->GetStats().allocations, 0u);
    EXPECT_EQ(server->GetStats().sessions, 0u);
    Challenge();
    now = 660000;
    server->Tick();
    EXPECT_EQ(server->GetStats().sessions, 0u);
}

TEST_F(TurnServerTest, OtherAccountCannotModifyAllocationAndUserQuotaIsEnforced)
{
    options.max_allocations_per_user = 1;
    StartServer();
    Allocate();
    ExpectCode(Exchange(Request(StunMethod::Refresh, {TurnCodec::UInt32Attribute(AttrType::LIFETIME, 0)}, Id(), "bob")), 441, "bob");
    EXPECT_EQ(server->GetStats().allocations, 1u);
    UdpPeer other;
    Challenge(&other);
    ExpectCode(Exchange(Request(StunMethod::Allocate, {TurnCodec::RequestedTransportAttribute(17)}, Id()), &other), 486);
    EXPECT_EQ(server->GetStats().allocations, 1u);
}

TEST_F(TurnServerTest, UnsupportedTransportUnknownAttributesAndPortExhaustionFailWithoutAllocation)
{
    Challenge();
    ExpectCode(Exchange(Request(StunMethod::Allocate, {TurnCodec::RequestedTransportAttribute(6)}, Id())), 442);
    ExpectCode(Exchange(Request(StunMethod::Allocate,
        {TurnCodec::RequestedTransportAttribute(17), {0x001A, {}}}, Id())), 420);
    EXPECT_EQ(server->GetStats().allocations, 0u);
    options.relay_port_min = options.relay_port_max = peer.address.Port();
    StartServer();
    Challenge();
    ExpectCode(Exchange(Request(StunMethod::Allocate, {TurnCodec::RequestedTransportAttribute(17)}, Id())), 508);
    EXPECT_EQ(server->GetStats().allocations, 0u);
}

TEST_F(TurnServerTest, PeerPolicyRejectsTargetsAndUnregisteredClientsCannotForward)
{
    Allocate();
    ExpectCode(Permission(network::SocketAddr::FromIPPort("224.0.0.1", 1234)), 403);
    ExpectCode(Permission(network::SocketAddr::FromIPPort("127.0.0.2", 1234)), 403);
    ExpectCode(BindChannel(0x4000, peer.address), 0);
    UdpPeer unknown;
    std::vector<uint8_t> channel, data;
    ASSERT_TRUE(TurnCodec::BuildChannelData(0x4000, "unauthorized", channel));
    ASSERT_TRUE(unknown.Send(control, channel));
    EXPECT_FALSE(peer.Receive(data, nullptr, 50));
    options.allow_peer = {};
    StartServer();
    Allocate();
    ExpectCode(Permission(peer.address), 403);
}

TEST_F(TurnServerTest, RealIceBindingCheckAndNominationCanTravelThroughRelay)
{
    Allocate();
    ExpectCode(Permission(peer.address), 0);
    IceRequestParams parameters;
    parameters.username = "sfu:browser";
    parameters.password = "sfu-password";
    parameters.priority = 1234;
    parameters.controlling = true;
    parameters.tie_breaker = 123;
    parameters.use_candidate = true;
    std::vector<uint8_t> binding(1500);
    size_t length = 0;
    ASSERT_TRUE(StunCodec::BuildIceBindingRequest(parameters, binding.data(), binding.size(), length));
    binding.resize(length);
    ASSERT_TRUE(client.Send(control, SendIndication(peer.address, binding)));
    std::vector<uint8_t> received, response;
    network::SocketAddr source;
    ASSERT_TRUE(peer.Receive(received, &source));
    ice::IceAgent sfu;
    sfu.SetLocalCredentials("sfu", "sfu-password");
    sfu.SetRemoteCredentials("browser", "browser-password");
    ASSERT_EQ(sfu.HandleDatagram(source, received.data(), received.size(), response), ice::IceAgent::HandleResult::SuccessResponse);
    EXPECT_TRUE(sfu.SelectedPeer() == relay);
    ASSERT_TRUE(peer.Send(source, response));
    ASSERT_TRUE(client.Receive(received));
    StunMessageInfo outer, inner;
    ASSERT_TRUE(TurnCodec::ParseStunDatagram(received.data(), received.size(), outer));
    const auto* attr = outer.FindAttr(static_cast<uint16_t>(AttrType::DATA));
    ASSERT_NE(attr, nullptr);
    const auto payload = outer.AttrValue(*attr);
    ASSERT_TRUE(TurnCodec::ParseStunDatagram(reinterpret_cast<const uint8_t*>(payload.data()), payload.size(), inner));
    EXPECT_TRUE(inner.IsBindingResponse());
    EXPECT_TRUE(StunCodec::VerifyMessageIntegrity(inner, "sfu-password"));
}

TEST_F(TurnServerTest, StopClosesAllSocketsAndInvalidConfigurationCannotStart)
{
    Allocate();
    server->Stop();
    EXPECT_EQ(server->GetStats().allocations, 0u);
    EXPECT_EQ(server->GetStats().sessions, 0u);
    const int fd = socket(AF_INET, SOCK_DGRAM, 0);
    ASSERT_GE(fd, 0);
    EXPECT_EQ(bind(fd, reinterpret_cast<sockaddr*>(&control.ss), control.len), 0);
    close(fd);
    options.advertised_ip = "0.0.0.0";
    auto invalid = std::make_shared<TurnServer>(scheduler, options, [](std::string_view, std::string&) { return false; });
    EXPECT_FALSE(invalid->Start());
}

TEST_F(TurnServerTest, UnsupportedAndMalformedRelayFamiliesAreRejected)
{
    Challenge();
    const auto family = [](std::vector<uint8_t> value) {
        return StunAttribute{static_cast<uint16_t>(AttrType::REQUESTED_ADDRESS_FAMILY), std::move(value)};
    };
    ExpectCode(Exchange(Request(StunMethod::Allocate,
        {TurnCodec::RequestedTransportAttribute(17), family({2, 0, 0, 0})}, Id())), 440);
    ExpectCode(Exchange(Request(StunMethod::Allocate,
        {TurnCodec::RequestedTransportAttribute(17), family({3, 0, 0, 0})}, Id())), 400);
    ExpectCode(Exchange(Request(StunMethod::Allocate,
        {TurnCodec::RequestedTransportAttribute(17), family({2})}, Id())), 400);
    ExpectCode(Exchange(Request(StunMethod::Allocate,
        {TurnCodec::RequestedTransportAttribute(17), family({1, 0, 0, 0}), family({2, 0, 0, 0})}, Id())), 400);
    EXPECT_EQ(server->GetStats().allocations, 0u);
}

// Bits select an IPv6 client and an IPv6 relay independently.
class TurnDualStackTest : public TurnServerTest, public testing::WithParamInterface<int>
{
protected:
    void SetUp() override
    {
        TurnServerTest::SetUp();
        client.Open(GetParam() & 1 ? "::1" : "127.0.0.1");
        peer.Open(GetParam() & 2 ? "::1" : "127.0.0.1");
        ASSERT_GT(client.address.Port(), 0);
        ASSERT_GT(peer.address.Port(), 0);
        options.listen_ip = "::";
        options.dual_stack = true;
        options.relay_bind_ip_v6 = options.advertised_ip_v6 = "::1";
        options.allow_peer = [](const network::SocketAddr& source) {
            return source.IsV6()
                ? IN6_IS_ADDR_LOOPBACK(&reinterpret_cast<const sockaddr_in6*>(&source.ss)->sin6_addr)
                : source.IPv4Bytes() == std::string("\x7f\x00\x00\x01", 4);
        };
        Restart();
    }
    void Restart()
    {
        StartServer();
        control = network::SocketAddr::FromIPPort(client.address.IsV6() ? "::1" : "127.0.0.1", control.Port());
    }
    uint8_t RelayFamily() const { return GetParam() & 2 ? 2 : 0; }
};

TEST_P(TurnDualStackTest, BindingUsesClientFamilyAndSharesListenerWithAllocate)
{
    const auto id = Id();
    const auto request = StunCodec::BuildBindingRequest(id);
    const auto response = Exchange(request);
    ExpectBinding(response, client, id);
    EXPECT_EQ(Exchange(request), response);
    EXPECT_EQ(server->GetStats().sessions, 0u);
    EXPECT_EQ(server->GetStats().allocations, 0u);
    Allocate(RelayFamily());
    ExpectBinding(Exchange(request), client, id);
    EXPECT_EQ(server->GetStats().sessions, 1u);
    EXPECT_EQ(server->GetStats().allocations, 1u);
}

TEST_P(TurnDualStackTest, AllocateIndicationsChannelsRefreshAndReleaseUseRealSockets)
{
    const auto request = Allocate(RelayFamily());
    const auto reply = Exchange(request);
    EXPECT_EQ(Exchange(request), reply);
    EXPECT_EQ(server->GetStats().sessions, 1u);
    EXPECT_EQ(server->GetStats().allocations, 1u);
    const std::vector<uint8_t> payload{0, 0x80, 0xff, 1};
    std::vector<uint8_t> received;
    ASSERT_TRUE(client.Send(control, SendIndication(peer.address, payload)));
    EXPECT_FALSE(peer.Receive(received, nullptr, 30));
    auto permission = peer.address;
    if (permission.IsV6()) reinterpret_cast<sockaddr_in6*>(&permission.ss)->sin6_port = 0;
    else reinterpret_cast<sockaddr_in*>(&permission.ss)->sin_port = 0;
    ExpectCode(Permission(permission), 0);
    ASSERT_TRUE(client.Send(control, SendIndication(peer.address, payload)));
    network::SocketAddr source;
    ASSERT_TRUE(peer.Receive(received, &source));
    EXPECT_TRUE(source == relay);
    EXPECT_EQ(received, payload);
    ASSERT_TRUE(peer.Send(relay, payload));
    ASSERT_TRUE(client.Receive(received));
    StunMessageInfo msg;
    ASSERT_TRUE(TurnCodec::ParseStunDatagram(received.data(), received.size(), msg));
    EXPECT_EQ(msg.method, StunMethod::Data);
    XorMappedAddress address;
    ASSERT_TRUE(StunCodec::DecodeXorAddress(msg, AttrType::XOR_PEER_ADDRESS, address));
    EXPECT_EQ(address.is_ipv6, peer.address.IsV6());
    EXPECT_EQ(address.port, peer.address.Port());
    const auto* attr = msg.FindAttr(static_cast<uint16_t>(AttrType::DATA));
    ASSERT_NE(attr, nullptr);
    EXPECT_EQ(msg.AttrValue(*attr), std::string(reinterpret_cast<const char*>(payload.data()), payload.size()));

    ExpectCode(BindChannel(0x4001, peer.address), 0);
    for (const auto& bytes : {payload, std::vector<uint8_t>{}})
    {
        std::vector<uint8_t> packet;
        ASSERT_TRUE(TurnCodec::BuildChannelData(0x4001,
            {reinterpret_cast<const char*>(bytes.data()), bytes.size()}, packet));
        ASSERT_TRUE(client.Send(control, packet));
        ASSERT_TRUE(peer.Receive(received));
        EXPECT_EQ(received, bytes);
        ASSERT_TRUE(peer.Send(relay, bytes));
        ASSERT_TRUE(client.Receive(received));
        TurnChannelDataView view;
        ASSERT_TRUE(TurnCodec::ParseChannelDataDatagram(received.data(), received.size(), view));
        EXPECT_EQ(view.channel, 0x4001);
        EXPECT_EQ(view.data, std::string_view(reinterpret_cast<const char*>(bytes.data()), bytes.size()));
    }
    now = 299999;
    ExpectCode(Exchange(Request(StunMethod::Refresh, {TurnCodec::UInt32Attribute(AttrType::LIFETIME, 600)}, Id())), 0);
    now = 300000;
    server->Tick();
    ASSERT_TRUE(peer.Send(relay, payload));
    EXPECT_FALSE(client.Receive(received, nullptr, 30));
    ExpectCode(Permission(peer.address), 0);
    ASSERT_TRUE(peer.Send(relay, payload));
    ASSERT_TRUE(client.Receive(received));
    ExpectCode(Exchange(Request(StunMethod::Refresh, {TurnCodec::UInt32Attribute(AttrType::LIFETIME, 0)}, Id())), 0);
    EXPECT_EQ(server->GetStats().allocations, 0u);
    // Use an unbound socket to check that deletion released the relay port.
    const int fd = socket(relay.ss.ss_family, SOCK_DGRAM, 0);
    ASSERT_GE(fd, 0);
    EXPECT_EQ(bind(fd, reinterpret_cast<const sockaddr*>(&relay.ss), relay.len), 0);
    close(fd);
}

TEST_P(TurnDualStackTest, PeerFamilyMismatchAndPrivatePeersCannotCreatePermissions)
{
    Allocate(RelayFamily());
    const auto wrong = network::SocketAddr::FromIPPort(peer.address.IsV6() ? "127.0.0.1" : "::1", 9000);
    ExpectCode(Permission(wrong), 443);
    ExpectCode(BindChannel(0x4000, wrong), 443);
    options.allow_peer = {};
    Restart();
    Allocate(RelayFamily());
    ExpectCode(Permission(peer.address), 403);
    const auto ips = peer.address.IsV6()
        ? std::vector<std::string>{"::", "fc00::1", "fe80::1", "ff02::1", "::ffff:127.0.0.1", "::127.0.0.1"}
        : std::vector<std::string>{"0.0.0.0", "10.0.0.1", "169.254.1.1", "224.0.0.1"};
    for (const auto& ip : ips)
        ExpectCode(Permission(network::SocketAddr::FromIPPort(ip, 9000)), 403);
}

INSTANTIATE_TEST_SUITE_P(AddressFamilies, TurnDualStackTest, testing::Values(0, 1, 2, 3),
    [](const testing::TestParamInfo<int>& info) {
        return std::string(info.param & 1 ? "ClientV6" : "ClientV4") +
            (info.param & 2 ? "RelayV6" : "RelayV4");
    });

TEST_F(TurnServerTest, IPv6OnlyListenerCanAllocateAndRelay)
{
    client.Open("::1");
    peer.Open("::1");
    ASSERT_GT(client.address.Port(), 0);
    ASSERT_GT(peer.address.Port(), 0);
    options.listen_ip = "::1";
    options.relay_bind_ip.clear();
    options.advertised_ip.clear();
    options.relay_bind_ip_v6 = options.advertised_ip_v6 = "::1";
    options.allow_peer = [](const network::SocketAddr& source) {
        return source.IsV6() && IN6_IS_ADDR_LOOPBACK(&reinterpret_cast<const sockaddr_in6*>(&source.ss)->sin6_addr);
    };
    StartServer();
    Challenge();
    ExpectCode(Exchange(Request(StunMethod::Allocate, {TurnCodec::RequestedTransportAttribute(17)}, Id())), 440);
    Allocate(2);
    ExpectCode(BindChannel(0x4000, peer.address), 0);
    ASSERT_TRUE(peer.Send(relay, {1, 2}));
    std::vector<uint8_t> received;
    ASSERT_TRUE(client.Receive(received));
    TurnChannelDataView view;
    ASSERT_TRUE(TurnCodec::ParseChannelDataDatagram(received.data(), received.size(), view));
    EXPECT_EQ(view.channel, 0x4000);
}

TEST_F(TurnServerTest, DualStackListenerKeepsSimultaneousClientsAndChannelsSeparate)
{
    UdpPeer client6("::1"), peer6("::1");
    ASSERT_GT(client6.address.Port(), 0);
    ASSERT_GT(peer6.address.Port(), 0);
    options.listen_ip = "0:0:0:0:0:0:0:0";
    options.dual_stack = true;
    options.relay_bind_ip_v6 = options.advertised_ip_v6 = "::1";
    options.allow_peer = [](const network::SocketAddr& source) {
        return source.IsV6()
            ? IN6_IS_ADDR_LOOPBACK(&reinterpret_cast<const sockaddr_in6*>(&source.ss)->sin6_addr)
            : source.IPv4Bytes() == std::string("\x7f\x00\x00\x01", 4);
    };
    StartServer();
    const auto control6 = network::SocketAddr::FromIPPort("::1", control.Port());
    control = network::SocketAddr::FromIPPort("127.0.0.1", control.Port());
    Allocate();
    ExpectCode(BindChannel(0x4000, peer.address), 0);
    const auto control4 = control;
    control = control6;
    Challenge(&client6);
    const auto response = Exchange(Request(StunMethod::Allocate,
        {TurnCodec::RequestedTransportAttribute(17),
         {static_cast<uint16_t>(AttrType::REQUESTED_ADDRESS_FAMILY), {2, 0, 0, 0}}}, Id()), &client6);
    ExpectCode(response, 0);
    StunMessageInfo msg;
    ASSERT_TRUE(TurnCodec::ParseStunDatagram(response.data(), response.size(), msg));
    XorMappedAddress address;
    ASSERT_TRUE(StunCodec::DecodeXorAddress(msg, AttrType::XOR_RELAYED_ADDRESS, address));
    const auto relay6 = network::SocketAddr::FromIPPort("::1", address.port);
    const auto id = Id();
    StunAttribute channel;
    ASSERT_TRUE(TurnCodec::ChannelNumberAttribute(0x4000, channel));
    ExpectCode(Exchange(Request(StunMethod::ChannelBind, {channel, PeerAttribute(peer6.address, id)}, id), &client6), 0);
    EXPECT_EQ(server->GetStats().sessions, 2u);
    EXPECT_EQ(server->GetStats().allocations, 2u);
    std::vector<uint8_t> packet4, packet6, received;
    ASSERT_TRUE(TurnCodec::BuildChannelData(0x4000, "ipv4", packet4));
    ASSERT_TRUE(TurnCodec::BuildChannelData(0x4000, "ipv6", packet6));
    ASSERT_TRUE(client.Send(control4, packet4));
    ASSERT_TRUE(client6.Send(control6, packet6));
    ASSERT_TRUE(peer.Receive(received));
    EXPECT_EQ(received, (std::vector<uint8_t>{'i', 'p', 'v', '4'}));
    ASSERT_TRUE(peer6.Receive(received));
    EXPECT_EQ(received, (std::vector<uint8_t>{'i', 'p', 'v', '6'}));
    ASSERT_TRUE(peer.Send(relay, {'4'}));
    ASSERT_TRUE(peer6.Send(relay6, {'6'}));
    ASSERT_TRUE(client.Receive(received));
    TurnChannelDataView view;
    ASSERT_TRUE(TurnCodec::ParseChannelDataDatagram(received.data(), received.size(), view));
    EXPECT_EQ(view.data, "4");
    ASSERT_TRUE(client6.Receive(received));
    ASSERT_TRUE(TurnCodec::ParseChannelDataDatagram(received.data(), received.size(), view));
    EXPECT_EQ(view.data, "6");
}

TEST(SocketAddrTest, IPv6ParsingMappedNormalizationAndScopeIdentity)
{
    EXPECT_FALSE(network::SocketAddr::FromIPPort("invalid", 3478).IsValid());
    const auto ipv6 = network::SocketAddr::FromIPPort("::1", 3478);
    ASSERT_TRUE(ipv6.IsV6());
    EXPECT_EQ(ipv6.ToString(), "[::1]:3478");
    const auto mapped = network::SocketAddr::FromIPPort("::ffff:127.0.0.1", 3478).Normalized();
    const auto ipv4 = network::SocketAddr::FromIPPort("127.0.0.1", 3478);
    EXPECT_TRUE(mapped == ipv4);
    EXPECT_EQ(network::SocketAddrHash{}(mapped), network::SocketAddrHash{}(ipv4));
    auto scoped = ipv6;
    reinterpret_cast<sockaddr_in6*>(&scoped.ss)->sin6_scope_id = 1;
    EXPECT_FALSE(scoped == ipv6);
}

TEST_F(TurnServerTest, InvalidDualStackAndRelayConfigurationsCannotStart)
{
    server->Stop();
    const auto invalid = [&](const TurnServerOptions& config) {
        auto candidate = std::make_shared<TurnServer>(scheduler, config,
            [](std::string_view, std::string& password) { password = "secret"; return true; });
        EXPECT_FALSE(candidate->Start());
    };
    auto config = options;
    config.dual_stack = true;
    invalid(config);
    config = options;
    config.relay_bind_ip_v6 = "::1";
    invalid(config);
    config.advertised_ip_v6 = "127.0.0.1";
    invalid(config);
    config.advertised_ip_v6 = "::";
    invalid(config);
    config.advertised_ip_v6 = "::ffff:127.0.0.1";
    invalid(config);
    config = options;
    config.relay_bind_ip = "invalid";
    invalid(config);
    config = options;
    config.relay_bind_ip.clear();
    config.advertised_ip.clear();
    invalid(config);
}

} // namespace protocol
