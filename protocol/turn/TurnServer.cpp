#include "TurnServer.h"

#include "CryptoUtil.h"
#include "TimeUtil.h"
#include "UdpServer.h"
#include "utils.h"

#include <algorithm>
#include <deque>
#include <limits>
#include <unordered_map>
#include <utility>

namespace protocol
{
namespace
{
constexpr uint64_t kPermissionMs = 300000;
constexpr uint64_t kChannelMs = 600000;
constexpr uint64_t kSessionIdleMs = 60000;
constexpr uint64_t kCacheMs = 40000;
constexpr size_t kMaxCachedResponses = 8;
constexpr size_t kMaxControlBytes = 4096;
constexpr uint32_t kDefaultLifetime = 600;
constexpr uint32_t kMaxLifetime = 3600;

bool UnicastV4(uint32_t host_ip)
{
    return (host_ip >> 24) != 0 && (host_ip >> 28) < 14;
}

bool Unicast(const network::SocketAddr& address)
{
    if (!address.IsValid()) return false;
    if (address.IsV4())
        return UnicastV4(ntohl(reinterpret_cast<const sockaddr_in*>(&address.ss)->sin_addr.s_addr));
    const auto* in6 = reinterpret_cast<const sockaddr_in6*>(&address.ss);
    return in6->sin6_scope_id == 0 && !IN6_IS_ADDR_UNSPECIFIED(&in6->sin6_addr) &&
        !IN6_IS_ADDR_MULTICAST(&in6->sin6_addr) && !IN6_IS_ADDR_V4MAPPED(&in6->sin6_addr);
}

bool RelayConfigValid(const std::string& bind_ip, const std::string& public_ip, int family)
{
    if (bind_ip.empty() && public_ip.empty()) return true;
    const auto bind = network::SocketAddr::FromIPPort(bind_ip, 0);
    const auto advertised = network::SocketAddr::FromIPPort(public_ip, 0);
    return bind.IsValid() && bind.ss.ss_family == family && advertised.ss.ss_family == family &&
        Unicast(advertised) && (family != AF_INET6 ||
            !IN6_IS_ADDR_V4MAPPED(&reinterpret_cast<const sockaddr_in6*>(&bind.ss)->sin6_addr));
}


/* 默认目标地址策略：过滤代码列出的私网、回环、链路本地、共享地址及基准测试网段 */
bool PublicPeer(const network::SocketAddr& peer)
{
    if (peer.IsV6())
    {
        const auto* ip = &reinterpret_cast<const sockaddr_in6*>(&peer.ss)->sin6_addr;
        return Unicast(peer) && !IN6_IS_ADDR_LOOPBACK(ip) && !IN6_IS_ADDR_LINKLOCAL(ip) &&
            (ip->s6_addr[0] & 0xfe) != 0xfc &&
            !(ip->s6_addr[0] == 0xfe && (ip->s6_addr[1] & 0xc0) == 0xc0) &&
            // Reject deprecated IPv4-compatible addresses as well as mapped addresses.
            !(std::all_of(ip->s6_addr, ip->s6_addr + 12, [](uint8_t b) { return b == 0; }));
    }
    const uint32_t ip = ntohl(reinterpret_cast<const sockaddr_in*>(&peer.ss)->sin_addr.s_addr);
    return UnicastV4(ip) && (ip >> 24) != 10 && (ip >> 24) != 127 &&
        (ip & 0xFFF00000u) != 0xAC100000u && (ip & 0xFFFF0000u) != 0xC0A80000u &&
        (ip & 0xFFFF0000u) != 0xA9FE0000u && (ip & 0xFFC00000u) != 0x64400000u &&
        (ip & 0xFFFE0000u) != 0xC6120000u;
}

/* Permission 按 IP 匹配，不包含对端端口。 */
network::SocketAddr PeerIp(network::SocketAddr peer)
{
    if (peer.IsV4()) reinterpret_cast<sockaddr_in*>(&peer.ss)->sin_port = 0;
    else reinterpret_cast<sockaddr_in6*>(&peer.ss)->sin6_port = 0;
    return peer;
}

/* 将 SocketAddr 转为编解码器所用的 IP 字节数组和端口 */
IpEndpoint Endpoint(const network::SocketAddr& addr)
{
    IpEndpoint ep;
    ep.port = addr.Port();
    ep.family = addr.IsV6() ? IpFamily::IPv6 : IpFamily::IPv4;
    const auto bytes = addr.IsV6() ? addr.IPv6Bytes() : addr.IPv4Bytes();
    std::copy(bytes.begin(), bytes.end(), ep.ip.begin());
    return ep;
}

uint64_t Deadline(uint64_t now, uint64_t duration)
{
    return duration > std::numeric_limits<uint64_t>::max() - now
        ? std::numeric_limits<uint64_t>::max() : now + duration;
}

/* 统计指定属性出现次数；当前解析策略只处理到 MESSAGE_INTEGRITY（含）为止 */
size_t AttributeCount(const StunMessageInfo& msg, AttrType type)
{
    size_t count = 0;
    for (const auto& attr : msg.attrs)
    {
        if (attr.type == static_cast<uint16_t>(type)) ++count;
        if (attr.type == static_cast<uint16_t>(AttrType::MESSAGE_INTEGRITY)) break;
    }
    return count;
}

// 没有 FINGERPRINT 时允许通过；存在时要求恰好一个且校验成功
// 指纹校验不是身份认证，身份和消息完整性另由 TurnAuth 处理
bool ValidFingerprint(const StunMessageInfo& msg)
{
    const auto count = std::count_if(msg.attrs.begin(), msg.attrs.end(), [](const AttrView& attr) {
        return attr.type == static_cast<uint16_t>(AttrType::FINGERPRINT);
    });
    return count == 0 || (count == 1 && StunCodec::VerifyFingerprint(msg));
}

/* 按请求方法列出本实现支持的属性；鉴权相关属性对非 Send 方法通用 */
bool AllowedAttribute(StunMethod method, uint16_t raw)
{
    const auto type = static_cast<AttrType>(raw);
    if (method == StunMethod::Binding)
    {
        // Basic Binding ignores recognized attributes from other usages (RFC 8489 6.3).
        switch (type)
        {
        case AttrType::MAPPED_ADDRESS:
        case AttrType::USERNAME:
        case AttrType::MESSAGE_INTEGRITY:
        case AttrType::MESSAGE_INTEGRITY_SHA256:
        case AttrType::ERROR_CODE:
        case AttrType::UNKNOWN_ATTRIBUTES:
        case AttrType::CHANNEL_NUMBER:
        case AttrType::LIFETIME:
        case AttrType::XOR_PEER_ADDRESS:
        case AttrType::DATA:
        case AttrType::REALM:
        case AttrType::NONCE:
        case AttrType::XOR_RELAYED_ADDRESS:
        case AttrType::REQUESTED_ADDRESS_FAMILY:
        case AttrType::REQUESTED_TRANSPORT:
        case AttrType::XOR_MAPPED_ADDRESS:
        case AttrType::PRIORITY:
        case AttrType::USE_CANDIDATE:
            return true;
        default:
            return false;
        }
    }
    if (method != StunMethod::Send && (type == AttrType::USERNAME || type == AttrType::REALM || type == AttrType::NONCE)) return true;
    if (method == StunMethod::Allocate)
        return type == AttrType::REQUESTED_TRANSPORT || type == AttrType::LIFETIME || type == AttrType::REQUESTED_ADDRESS_FAMILY;
    if (method == StunMethod::Refresh) return type == AttrType::LIFETIME;
    if (method == StunMethod::CreatePermission) return type == AttrType::XOR_PEER_ADDRESS;
    if (method == StunMethod::ChannelBind)
        return type == AttrType::XOR_PEER_ADDRESS || type == AttrType::CHANNEL_NUMBER;
    if (method == StunMethod::Send) return type == AttrType::XOR_PEER_ADDRESS || type == AttrType::DATA;
    return false;
}

/* 收集并去重不支持的必需理解属性（类型值小于 0x8000） */
std::vector<uint16_t> UnknownAttributes(const StunMessageInfo& msg)
{
    std::vector<uint16_t> result;
    for (const auto& attr : msg.attrs)
    {
        if (attr.type == static_cast<uint16_t>(AttrType::MESSAGE_INTEGRITY) ||
            (msg.method == StunMethod::Binding && attr.type == static_cast<uint16_t>(AttrType::MESSAGE_INTEGRITY_SHA256))) break;
        if (attr.type < 0x8000 && !AllowedAttribute(msg.method, attr.type) &&
            std::find(result.begin(), result.end(), attr.type) == result.end()) result.push_back(attr.type);
    }
    return result;
}
} // namespace

// TURN 内部状态容器（PImpl）。控制 socket、会话和定时任务集中由所属 scheduler 线程处理，避免多线程并发访问。

struct TurnServer::Impl
{
    struct Channel
    {
        network::SocketAddr peer;
        uint64_t expires = 0;
    };
    struct Allocation
    {
        ~Allocation() { if (relay) relay->Stop(); }
        std::shared_ptr<network::UdpServer> relay;
        network::SocketAddr advertised;
        std::string username;
        std::array<uint8_t, 12> transaction{};
        std::vector<uint8_t> request;
        uint64_t generation = 0;
        uint64_t expires = 0;
        std::unordered_map<network::SocketAddr, uint64_t, network::SocketAddrHash> permissions;
        std::unordered_map<uint16_t, Channel> channels;
    };
    struct CachedResponse
    {
        std::array<uint8_t, 12> transaction{};
        std::vector<uint8_t> request;
        std::vector<uint8_t> response;
        uint64_t expires = 0;
    };
    struct Session
    {
        TurnAuthContext auth;
        std::unique_ptr<Allocation> allocation;
        std::deque<CachedResponse> cache;
        uint64_t last_activity = 0;
    };

    std::shared_ptr<TaskScheduler> scheduler;
    TurnServerOptions options;
    TurnAuth::Clock clock;
    TurnAuth auth;
    std::shared_ptr<network::UdpServer> control;
    std::unordered_map<network::SocketAddr, Session, network::SocketAddrHash> sessions;
    std::weak_ptr<TurnServer> owner;
    TimeId timer = 0;
    uint32_t next_port = 0;
    uint64_t generation = 0;
    Stats stats;
    bool started = false;

    Impl(std::shared_ptr<TaskScheduler> scheduler, TurnServerOptions options, TurnAuth::PasswordLookup lookup)
        : scheduler(std::move(scheduler)), options(std::move(options)),
          clock(this->options.clock ? this->options.clock : TurnAuth::Clock(Timestamp::NowMs)),
          auth(this->options.realm, std::move(lookup), 600000, clock), next_port(this->options.relay_port_min) {}

    bool PeerAllowed(const network::SocketAddr& peer, bool require_port = true) const
    {
        return Unicast(peer) && (!require_port || peer.Port() != 0) &&
            (options.allow_peer ? options.allow_peer(peer) : PublicPeer(peer));
    }

    void Prune(uint64_t now)
    {
        for (auto it = sessions.begin(); it != sessions.end();)
        {
            auto& session = it->second;
            while (!session.cache.empty() && now >= session.cache.front().expires) session.cache.pop_front();
            if (session.allocation && now >= session.allocation->expires) session.allocation.reset();
            if (auto* allocation = session.allocation.get())
            {
                for (auto p = allocation->permissions.begin(); p != allocation->permissions.end();)
                    if (now >= p->second) p = allocation->permissions.erase(p); else ++p;
                for (auto c = allocation->channels.begin(); c != allocation->channels.end();)
                    if (now >= c->second.expires) c = allocation->channels.erase(c); else ++c;
            }
            if (!session.allocation && now >= session.last_activity &&
                now - session.last_activity >= kSessionIdleMs) it = sessions.erase(it);
            else ++it;
        }
    }

    size_t AllocationCount(std::string_view username = {}) const
    {
        size_t count = 0;
        for (const auto& item : sessions)
            if (item.second.allocation && (username.empty() || item.second.allocation->username == username)) ++count;
        return count;
    }

    bool HasPermission(const Allocation& allocation, const network::SocketAddr& peer) const
    {
        const auto p = allocation.permissions.find(PeerIp(peer));
        return p != allocation.permissions.end() && clock() < p->second;
    }

    uint16_t DecodePeer(const StunMessageInfo& msg, const AttrView& attr, const Allocation& allocation,
                        network::SocketAddr& out) const
    {
        // Decode this particular attribute, including repeated peer addresses.
        auto view = msg;
        view.attrs = {attr};
        XorMappedAddress decoded;
        if (!StunCodec::DecodeXorAddress(view, AttrType::XOR_PEER_ADDRESS, decoded)) return 400;
        if (decoded.is_ipv6 != allocation.advertised.IsV6()) return 443;
        if (decoded.is_ipv6)
        {
            sockaddr_in6 address{};
            address.sin6_family = AF_INET6;
            address.sin6_port = htons(decoded.port);
            std::copy_n(decoded.ip.begin(), 16, address.sin6_addr.s6_addr);
            out = network::SocketAddr::FromSockaddr(reinterpret_cast<sockaddr*>(&address), sizeof(address));
        }
        else
        {
            sockaddr_in address{};
            address.sin_family = AF_INET;
            address.sin_port = htons(decoded.port);
            std::copy_n(decoded.ip.begin(), 4, reinterpret_cast<uint8_t*>(&address.sin_addr));
            out = network::SocketAddr::FromSockaddr(reinterpret_cast<sockaddr*>(&address), sizeof(address));
        }
        return PeerAllowed(out, msg.method != StunMethod::CreatePermission) ? 0 : 403;
    }

    std::vector<uint8_t> Success(const StunMessageInfo& msg, std::string_view key,
                                const std::vector<StunAttribute>& attributes = {}) const
    {
        std::vector<uint8_t> response;
        StunCodec::BuildMessage(msg.method, StunClass::SuccessResponse, msg.txid, attributes, response, key, true);
        return response;
    }

    std::vector<uint8_t> Error(const StunMessageInfo& msg, uint16_t code, std::string_view key,
                              const std::vector<uint16_t>& unknown = {}) const
    {
        std::vector<StunAttribute> attributes = {{static_cast<uint16_t>(AttrType::ERROR_CODE),
            {0, 0, uint8_t(code / 100), uint8_t(code % 100)}}};
        if (!unknown.empty())
        {
            StunAttribute attr{static_cast<uint16_t>(AttrType::UNKNOWN_ATTRIBUTES), {}};
            for (auto value : unknown)
            {
                attr.value.push_back(uint8_t(value >> 8));
                attr.value.push_back(uint8_t(value));
            }
            attributes.push_back(std::move(attr));
        }
        std::vector<uint8_t> response;
        StunCodec::BuildMessage(msg.method, StunClass::ErrorResponse, msg.txid, attributes, response, key, true);
        return response;
    }

    std::shared_ptr<network::UdpServer> OpenRelay(IpFamily family);
    std::vector<uint8_t> HandleRequest(const network::SocketAddr& client, Session& session,
        const StunMessageInfo& msg, const TurnAuthResult& authenticated, const uint8_t* data, size_t len);
    void OnControl(const network::SocketAddr& client, const uint8_t* data, size_t len);
    void OnRelay(const network::SocketAddr& client, uint64_t id,
                 const network::SocketAddr& peer, const uint8_t* data, size_t len);
};

class TurnServer::ControlHandler final : public network::IUdpHandler
{
public:
    explicit ControlHandler(std::weak_ptr<TurnServer> server) : server_(std::move(server)) {}
    void OnDatagram(const network::SocketAddr& src, const uint8_t* data, size_t len) override
    {
        if (auto server = server_.lock()) server->impl_->OnControl(src, data, len);
    }
private:
    std::weak_ptr<TurnServer> server_;
};

class TurnServer::RelayHandler final : public network::IUdpHandler
{
public:
    RelayHandler(std::weak_ptr<TurnServer> server, network::SocketAddr client, uint64_t id)
        : server_(std::move(server)), client_(client), id_(id) {}
    void OnDatagram(const network::SocketAddr& src, const uint8_t* data, size_t len) override
    {
        if (auto server = server_.lock()) server->impl_->OnRelay(client_, id_, src, data, len);
    }
private:
    std::weak_ptr<TurnServer> server_;
    network::SocketAddr client_;
    uint64_t id_;
};

std::shared_ptr<network::UdpServer> TurnServer::Impl::OpenRelay(IpFamily family)
{
    const uint32_t attempts = options.relay_port_min == 0 ? 1 :
        uint32_t(options.relay_port_max) - options.relay_port_min + 1;
    for (uint32_t i = 0; i < attempts; ++i)
    {
        const uint16_t port = static_cast<uint16_t>(next_port);
        if (options.relay_port_min != 0)
            next_port = next_port == options.relay_port_max ? options.relay_port_min : next_port + 1;
        auto relay = std::make_shared<network::UdpServer>(scheduler);
        const auto& bind_ip = family == IpFamily::IPv6 ? options.relay_bind_ip_v6 : options.relay_bind_ip;
        if (relay->Start(bind_ip, port, false)) return relay;
    }
    return {};
}

std::vector<uint8_t> TurnServer::Impl::HandleRequest(const network::SocketAddr& client, Session& session,
    const StunMessageInfo& msg, const TurnAuthResult& authenticated, const uint8_t* data, size_t len)
{
    const auto key = std::string_view(authenticated.integrity_key);
    auto* allocation = session.allocation.get();
    if (allocation && allocation->username != authenticated.username)
        return Error(msg, msg.method == StunMethod::Allocate ? 437 : 441, key);
    const auto unknown = UnknownAttributes(msg);
    if (!unknown.empty()) return Error(msg, 420, key, unknown);
    const uint64_t now = clock();
    if (msg.method == StunMethod::Allocate || msg.method == StunMethod::Refresh)
    {
        uint32_t lifetime = kDefaultLifetime;
        if (AttributeCount(msg, AttrType::LIFETIME) > 1) return Error(msg, 400, key);
        if (AttributeCount(msg, AttrType::LIFETIME) && !TurnCodec::DecodeUInt32(msg, AttrType::LIFETIME, lifetime))
            return Error(msg, 400, key);
        if (msg.method == StunMethod::Refresh)
        {
            if (!allocation) return Error(msg, 437, key);
            if (lifetime != 0) lifetime = std::clamp(lifetime, kDefaultLifetime, kMaxLifetime);
            auto response = Success(msg, key, {TurnCodec::UInt32Attribute(AttrType::LIFETIME, lifetime)});
            if (response.empty()) return {};
            if (lifetime == 0) session.allocation.reset();
            else allocation->expires = Deadline(now, uint64_t(lifetime) * 1000);
            return response;
        }
        uint8_t transport = 0;
        if (AttributeCount(msg, AttrType::REQUESTED_TRANSPORT) != 1 ||
            !TurnCodec::DecodeRequestedTransport(msg, transport)) return Error(msg, 400, key);
        if (transport != TurnCodec::kUdpTransport) return Error(msg, 442, key);
        IpFamily requested_family = IpFamily::IPv4;
        if (AttributeCount(msg, AttrType::REQUESTED_ADDRESS_FAMILY) > 1) return Error(msg, 400, key);
        if (const auto* family = msg.FindAttr(static_cast<uint16_t>(AttrType::REQUESTED_ADDRESS_FAMILY)))
        {
            if (family->len != 4) return Error(msg, 400, key);
            const auto value = msg.raw[family->value_offset];
            if (value != 1 && value != 2) return Error(msg, 400, key);
            requested_family = value == 2 ? IpFamily::IPv6 : IpFamily::IPv4;
        }
        const auto& public_ip = requested_family == IpFamily::IPv6 ? options.advertised_ip_v6 : options.advertised_ip;
        if (public_ip.empty()) return Error(msg, 440, key);
        lifetime = std::clamp(lifetime, kDefaultLifetime, kMaxLifetime);
        std::unique_ptr<Allocation> pending;
        if (allocation)
        {
            if (allocation->transaction != msg.txid) return Error(msg, 437, key);
            if (allocation->request != std::vector<uint8_t>(data, data + len)) return Error(msg, 400, key);
            lifetime = static_cast<uint32_t>((allocation->expires - now + 999) / 1000);
        }
        else
        {
            if (AllocationCount() >= options.max_allocations ||
                AllocationCount(authenticated.username) >= options.max_allocations_per_user) return Error(msg, 486, key);
            pending = std::make_unique<Allocation>();
            pending->relay = OpenRelay(requested_family);
            if (!pending->relay) return Error(msg, 508, key);
            pending->advertised = network::SocketAddr::FromIPPort(public_ip, pending->relay->LocalAddress().Port());
            pending->username = authenticated.username;
            pending->transaction = msg.txid;
            pending->request.assign(data, data + len);
            pending->expires = Deadline(now, uint64_t(lifetime) * 1000);
            pending->generation = ++generation;
            pending->relay->SetHandler(std::make_shared<RelayHandler>(owner, client, pending->generation));
            allocation = pending.get();
        }
        StunAttribute relay, mapped;
        if (!TurnCodec::XorAddressAttribute(AttrType::XOR_RELAYED_ADDRESS, Endpoint(allocation->advertised), msg.txid, relay) ||
            !TurnCodec::XorAddressAttribute(AttrType::XOR_MAPPED_ADDRESS, Endpoint(client), msg.txid, mapped)) return {};
        auto response = Success(msg, key, {relay, mapped, TurnCodec::UInt32Attribute(AttrType::LIFETIME, lifetime)});
        if (!response.empty() && pending) session.allocation = std::move(pending);
        return response;
    }
    if (!allocation) return Error(msg, 437, key);

    std::vector<network::SocketAddr> peers;
    for (const auto& attr : msg.attrs)
    {
        if (attr.type == static_cast<uint16_t>(AttrType::MESSAGE_INTEGRITY)) break;
        if (attr.type != static_cast<uint16_t>(AttrType::XOR_PEER_ADDRESS)) continue;
        network::SocketAddr peer;
        if (const auto error = DecodePeer(msg, attr, *allocation, peer)) return Error(msg, error, key);
        peers.push_back(peer);
    }
    if (peers.empty()) return Error(msg, 400, key);
    if (msg.method == StunMethod::ChannelBind && peers.size() != 1) return Error(msg, 400, key);
    auto permissions = allocation->permissions;
    for (const auto& peer : peers) permissions[PeerIp(peer)] = Deadline(now, kPermissionMs);
    if (permissions.size() > options.max_permissions) return Error(msg, 508, key);
    if (msg.method == StunMethod::ChannelBind)
    {
        uint16_t channel = 0;
        if (AttributeCount(msg, AttrType::CHANNEL_NUMBER) != 1 || !TurnCodec::DecodeChannelNumber(msg, channel))
            return Error(msg, 400, key);
        const auto existing = allocation->channels.find(channel);
        if (existing != allocation->channels.end() && !(existing->second.peer == peers.front())) return Error(msg, 400, key);
        for (const auto& binding : allocation->channels)
            if (binding.first != channel && binding.second.peer == peers.front()) return Error(msg, 400, key);
        auto channels = allocation->channels;
        channels[channel] = {peers.front(), Deadline(now, kChannelMs)};
        if (channels.size() > options.max_channels) return Error(msg, 508, key);
        auto response = Success(msg, key);
        if (response.empty()) return {};
        allocation->permissions.swap(permissions);
        allocation->channels.swap(channels);
        return response;
    }
    auto response = Success(msg, key);
    if (!response.empty()) allocation->permissions.swap(permissions);
    return response;
}

void TurnServer::Impl::OnControl(const network::SocketAddr& client, const uint8_t* data, size_t len)
try
{
    if (!started || !data || len == 0 || len > 65507 || !client.IsValid()) return;
    const auto now = clock();
    Prune(now);
    auto session_it = sessions.find(client);
    TurnChannelDataView channel_data;
    if (TurnCodec::ParseChannelDataDatagram(data, len, channel_data))
    {
        if (session_it == sessions.end() || !session_it->second.allocation) return;
        auto& allocation = *session_it->second.allocation;
        const auto binding = allocation.channels.find(channel_data.channel);
        if (binding == allocation.channels.end() || !PeerAllowed(binding->second.peer)) return;
        if (allocation.relay->SendTo(binding->second.peer, reinterpret_cast<const uint8_t*>(channel_data.data.data()), channel_data.data.size())) ++stats.client_to_peer;
        return;
    }
    StunMessageInfo msg;
    if (!TurnCodec::ParseStunDatagram(data, len, msg) || !ValidFingerprint(msg)) return;
    if (msg.method == StunMethod::Binding)
    {
        if (!msg.IsBindingRequest() || len > kMaxControlBytes) return;
        const auto unknown = UnknownAttributes(msg);
        std::vector<uint8_t> response;
        if (!unknown.empty()) response = Error(msg, 420, {}, unknown);
        else
        {
            StunAttribute mapped;
            if (!TurnCodec::XorAddressAttribute(AttrType::XOR_MAPPED_ADDRESS, Endpoint(client), msg.txid, mapped)) return;
            response = Success(msg, {}, {mapped});
        }
        // Public address discovery is stateless; retries recompute the same response.
        if (!response.empty()) control->SendTo(client, response.data(), response.size());
        return;
    }
    if (msg.method == StunMethod::Send && msg.klass == StunClass::Indication)
    {
        if (session_it == sessions.end() || !session_it->second.allocation || !UnknownAttributes(msg).empty() || AttributeCount(msg, AttrType::XOR_PEER_ADDRESS) != 1 || AttributeCount(msg, AttrType::DATA) != 1) return;
        network::SocketAddr peer;
        const auto* attr = msg.FindAttr(static_cast<uint16_t>(AttrType::XOR_PEER_ADDRESS));
        if (DecodePeer(msg, *attr, *session_it->second.allocation, peer) || !HasPermission(*session_it->second.allocation, peer)) return;
        const auto payload = msg.AttrValue(*msg.FindAttr(static_cast<uint16_t>(AttrType::DATA)));
        if (session_it->second.allocation->relay->SendTo(peer,
            reinterpret_cast<const uint8_t*>(payload.data()), payload.size())) ++stats.client_to_peer;
        return;
    }
    if (msg.klass != StunClass::Request || len > kMaxControlBytes ||
        (msg.method != StunMethod::Allocate && msg.method != StunMethod::Refresh &&
         msg.method != StunMethod::CreatePermission && msg.method != StunMethod::ChannelBind)) return;
    if (session_it == sessions.end())
    {
        if (sessions.size() >= options.max_sessions) return;
        session_it = sessions.try_emplace(client).first;
    }
    auto& session = session_it->second;
    session.last_activity = now;
    for (const auto& cached : session.cache)
    {
        if (cached.transaction != msg.txid) continue;
        if (cached.request.size() == len && std::equal(cached.request.begin(), cached.request.end(), data))
            control->SendTo(client, cached.response.data(), cached.response.size());
        return;
    }
    auto authenticated = auth.Authenticate(data, len, session.auth);
    std::vector<uint8_t> response;
    if (authenticated.status == TurnAuthResult::Status::Response) response = std::move(authenticated.response);
    else if (authenticated.status == TurnAuthResult::Status::Authorized)
        response = HandleRequest(client, session, msg, authenticated, data, len);
    if (response.empty()) return;
    session.cache.push_back({msg.txid, {data, data + len}, std::move(response), Deadline(now, kCacheMs)});
    if (session.cache.size() > kMaxCachedResponses) session.cache.pop_front();
    const auto& reply = session.cache.back().response;
    control->SendTo(client, reply.data(), reply.size());
}
catch (const std::bad_alloc&) {} // Drop under memory pressure; pending allocations own their sockets.

void TurnServer::Impl::OnRelay(const network::SocketAddr& client, uint64_t id, const network::SocketAddr& peer, const uint8_t* data, size_t len)
try
{
    if (!started || !PeerAllowed(peer)) return;
    Prune(clock());
    const auto found = sessions.find(client);
    if (found == sessions.end() || !found->second.allocation) return;
    const auto& allocation = *found->second.allocation;
    if (allocation.generation != id || !HasPermission(allocation, peer)) return;
    std::vector<uint8_t> packet;
    for (const auto& binding : allocation.channels)
    {
        if (!(binding.second.peer == peer)) continue;
        if (TurnCodec::BuildChannelData(binding.first, {reinterpret_cast<const char*>(data), len}, packet) &&
            control->SendTo(client, packet.data(), packet.size())) ++stats.peer_to_client;
        return;
    }
    std::array<uint8_t, 12> txid{};
    if (!utils::SecureRandomBytes(txid.data(), txid.size())) return;
    StunAttribute address;
    if (!TurnCodec::XorAddressAttribute(AttrType::XOR_PEER_ADDRESS, Endpoint(peer), txid, address)) return;
    StunAttribute payload{static_cast<uint16_t>(AttrType::DATA), {data, data + len}};
    if (StunCodec::BuildMessage(StunMethod::Data, StunClass::Indication, txid, {address, payload}, packet) &&
        control->SendTo(client, packet.data(), packet.size())) ++stats.peer_to_client;
}
catch (const std::bad_alloc&) {}

TurnServer::TurnServer(std::shared_ptr<TaskScheduler> scheduler, TurnServerOptions options,
                       TurnAuth::PasswordLookup lookup)
    : impl_(std::make_unique<Impl>(std::move(scheduler), std::move(options), std::move(lookup))) {}

TurnServer::~TurnServer() { Stop(); }

bool TurnServer::Start()
{
    if (!impl_->scheduler || impl_->scheduler->IsStopped() || weak_from_this().expired()) return false;
    bool result = false;
    impl_->scheduler->Invoke([&] {
        auto& state = *impl_;
        const auto listen = network::SocketAddr::FromIPPort(state.options.listen_ip, state.options.listen_port);
        if (state.started || !state.auth.IsConfigured() ||
            !listen.IsValid() || (state.options.dual_stack && (!listen.IsV6() ||
                !IN6_IS_ADDR_UNSPECIFIED(&reinterpret_cast<const sockaddr_in6*>(&listen.ss)->sin6_addr))) ||
            !RelayConfigValid(state.options.relay_bind_ip, state.options.advertised_ip, AF_INET) ||
            !RelayConfigValid(state.options.relay_bind_ip_v6, state.options.advertised_ip_v6, AF_INET6) ||
            (state.options.advertised_ip.empty() && state.options.advertised_ip_v6.empty()) ||
            (state.options.relay_port_min == 0 ? state.options.relay_port_max != 0 :
             state.options.relay_port_min > state.options.relay_port_max) ||
            !state.options.max_sessions || !state.options.max_allocations || !state.options.max_allocations_per_user ||
            !state.options.max_permissions || !state.options.max_channels) return;
        state.owner = weak_from_this();
        state.control = std::make_shared<network::UdpServer>(state.scheduler);
        if (!state.control->Start(state.options.listen_ip, state.options.listen_port, false, state.options.dual_stack)) return;
        state.control->SetHandler(std::make_shared<ControlHandler>(state.owner));
        state.started = true;
        state.timer = state.scheduler->AddTimer([weak = state.owner] {
            auto server = weak.lock();
            if (!server || !server->impl_->started) return false;
            server->impl_->Prune(server->impl_->clock());
            return true;
        }, 1000);
        if (!state.timer) { Stop(); return; }
        result = true;
    });
    return result;
}

void TurnServer::Stop()
{
    if (!impl_->scheduler) return;
    impl_->scheduler->Invoke([this] {
        auto& state = *impl_;
        state.started = false;
        if (state.timer) { state.scheduler->RemoveTimer(state.timer); state.timer = 0; }
        state.sessions.clear();
        if (state.control) { state.control->SetHandler({}); state.control->Stop(); state.control.reset(); }
    });
}

void TurnServer::Tick()
{
    if (impl_->scheduler) impl_->scheduler->Invoke([this] { impl_->Prune(impl_->clock()); });
}

network::SocketAddr TurnServer::LocalAddress() const
{
    network::SocketAddr address;
    if (impl_->scheduler) impl_->scheduler->Invoke([&] {
        if (impl_->control) address = impl_->control->LocalAddress();
    });
    return address;
}

TurnServer::Stats TurnServer::GetStats() const
{
    Stats stats;
    if (impl_->scheduler) impl_->scheduler->Invoke([&] {
        stats = impl_->stats;
        stats.sessions = impl_->sessions.size();
        stats.allocations = impl_->AllocationCount();
    });
    return stats;
}

} // namespace protocol
