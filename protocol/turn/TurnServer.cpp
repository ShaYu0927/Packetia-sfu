#include "TurnServer.h"
#include "TurnAuthRateLimiter.h"

#include "CryptoUtil.h"
#include "TimeUtil.h"
#include "UdpServer.h"
#include "utils.h"
#include "logger.h"

#include <algorithm>
#include <climits>
#include <deque>
#include <limits>
#include <unordered_map>
#include <utility>

// Numeric prefixes for the existing default peer ACL (host byte order).
#define PACKETIA_TURN_IPV4_FIRST_OCTET_SHIFT 24u
#define PACKETIA_TURN_IPV4_CLASS_SHIFT 28u
#define PACKETIA_TURN_IPV4_MULTICAST_CLASS 14u
#define PACKETIA_TURN_IPV4_PRIVATE_10_FIRST_OCTET 10u
#define PACKETIA_TURN_IPV4_PREFIX_12_MASK 0xFFF00000u
#define PACKETIA_TURN_IPV4_PREFIX_16_MASK 0xFFFF0000u
#define PACKETIA_TURN_IPV4_PREFIX_10_MASK 0xFFC00000u
#define PACKETIA_TURN_IPV4_PREFIX_15_MASK 0xFFFE0000u
#define PACKETIA_TURN_IPV4_PRIVATE_172_NETWORK 0xAC100000u
#define PACKETIA_TURN_IPV4_PRIVATE_192_NETWORK 0xC0A80000u
#define PACKETIA_TURN_IPV4_LINK_LOCAL_NETWORK 0xA9FE0000u
#define PACKETIA_TURN_IPV4_SHARED_NETWORK 0x64400000u
#define PACKETIA_TURN_IPV4_BENCHMARK_NETWORK 0xC6120000u
#define PACKETIA_TURN_IPV6_UNIQUE_LOCAL_MASK 0xfeu
#define PACKETIA_TURN_IPV6_UNIQUE_LOCAL_PREFIX 0xfcu
#define PACKETIA_TURN_IPV6_SITE_LOCAL_FIRST_BYTE 0xfeu
#define PACKETIA_TURN_IPV6_SITE_LOCAL_SECOND_MASK 0xc0u

namespace protocol
{
namespace
{
constexpr uint64_t kPermissionMs = PACKETIA_TURN_PERMISSION_LIFETIME_MS;
constexpr uint64_t kChannelMs = PACKETIA_TURN_CHANNEL_LIFETIME_MS;
constexpr uint64_t kSessionIdleMs = PACKETIA_TURN_SESSION_IDLE_MS;
constexpr uint64_t kCacheMs = PACKETIA_TURN_RESPONSE_CACHE_MS;
constexpr size_t kMaxCachedResponses = PACKETIA_TURN_MAX_CACHED_RESPONSES;
constexpr size_t kMaxControlBytes = PACKETIA_TURN_MAX_CONTROL_BYTES;
constexpr uint32_t kDefaultLifetime = PACKETIA_TURN_DEFAULT_ALLOCATION_LIFETIME_SECONDS;
constexpr uint32_t kMaxLifetime = PACKETIA_TURN_MAX_ALLOCATION_LIFETIME_SECONDS;

bool UnicastV4(uint32_t host_ip)
{
    return (host_ip >> PACKETIA_TURN_IPV4_FIRST_OCTET_SHIFT) != 0 &&
        (host_ip >> PACKETIA_TURN_IPV4_CLASS_SHIFT) < PACKETIA_TURN_IPV4_MULTICAST_CLASS;
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
        const bool site_local = ip->s6_addr[0] == PACKETIA_TURN_IPV6_SITE_LOCAL_FIRST_BYTE &&
            (ip->s6_addr[1] & PACKETIA_TURN_IPV6_SITE_LOCAL_SECOND_MASK) == PACKETIA_TURN_IPV6_SITE_LOCAL_SECOND_MASK;
        const auto* compatible_prefix_end = ip->s6_addr +
            (PACKETIA_STUN_IPV6_ADDRESS_BYTES - PACKETIA_STUN_IPV4_ADDRESS_BYTES);
        return Unicast(peer) && !IN6_IS_ADDR_LOOPBACK(ip) && !IN6_IS_ADDR_LINKLOCAL(ip) &&
            (ip->s6_addr[0] & PACKETIA_TURN_IPV6_UNIQUE_LOCAL_MASK) != PACKETIA_TURN_IPV6_UNIQUE_LOCAL_PREFIX &&
            !site_local &&
            // Reject deprecated IPv4-compatible addresses as well as mapped addresses.
            !std::all_of(ip->s6_addr, compatible_prefix_end, [](uint8_t b) { return b == 0; });
    }
    const uint32_t ip = ntohl(reinterpret_cast<const sockaddr_in*>(&peer.ss)->sin_addr.s_addr);
    const auto first_octet = ip >> PACKETIA_TURN_IPV4_FIRST_OCTET_SHIFT;
    return UnicastV4(ip) && first_octet != PACKETIA_TURN_IPV4_PRIVATE_10_FIRST_OCTET &&
        first_octet != PACKETIA_TURN_IPV4_LOOPBACK_FIRST_OCTET &&
        (ip & PACKETIA_TURN_IPV4_PREFIX_12_MASK) != PACKETIA_TURN_IPV4_PRIVATE_172_NETWORK &&
        (ip & PACKETIA_TURN_IPV4_PREFIX_16_MASK) != PACKETIA_TURN_IPV4_PRIVATE_192_NETWORK &&
        (ip & PACKETIA_TURN_IPV4_PREFIX_16_MASK) != PACKETIA_TURN_IPV4_LINK_LOCAL_NETWORK &&
        (ip & PACKETIA_TURN_IPV4_PREFIX_10_MASK) != PACKETIA_TURN_IPV4_SHARED_NETWORK &&
        (ip & PACKETIA_TURN_IPV4_PREFIX_15_MASK) != PACKETIA_TURN_IPV4_BENCHMARK_NETWORK;
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
        if (attr.type < PACKETIA_STUN_OPTIONAL_ATTRIBUTE_START && !AllowedAttribute(msg.method, attr.type) &&
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
        std::array<uint8_t, PACKETIA_STUN_TRANSACTION_ID_BYTES> transaction{};
        std::vector<uint8_t> request;
        uint64_t generation = 0;
        uint64_t expires = 0;
        std::unordered_map<network::SocketAddr, uint64_t, network::SocketAddrHash> permissions;
        std::unordered_map<uint16_t, Channel> channels;
    };
    struct CachedResponse
    {
        std::array<uint8_t, PACKETIA_STUN_TRANSACTION_ID_BYTES> transaction{};
        std::vector<uint8_t> request;
        std::vector<uint8_t> response;
        uint64_t expires = 0;
        bool unauthorized = false;
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
    TurnAuthRateLimiter unauthorized_limiter;
    bool started = false;

    Impl(std::shared_ptr<TaskScheduler> scheduler, TurnServerOptions options, TurnAuth::PasswordLookup lookup)
        : scheduler(std::move(scheduler)), options(std::move(options)),
          clock(this->options.clock ? this->options.clock : TurnAuth::Clock(Timestamp::NowMs)),
          auth(this->options.realm, std::move(lookup), PACKETIA_TURN_NONCE_LIFETIME_MS, clock), next_port(this->options.relay_port_min) {}

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
        if (!StunCodec::DecodeXorAddress(view, AttrType::XOR_PEER_ADDRESS, decoded)) return PACKETIA_STUN_ERROR_BAD_REQUEST;
        if (decoded.is_ipv6 != allocation.advertised.IsV6()) return PACKETIA_STUN_ERROR_PEER_ADDRESS_FAMILY_MISMATCH;
        if (decoded.is_ipv6)
        {
            sockaddr_in6 address{};
            address.sin6_family = AF_INET6;
            address.sin6_port = htons(decoded.port);
            std::copy_n(decoded.ip.begin(), PACKETIA_STUN_IPV6_ADDRESS_BYTES, address.sin6_addr.s6_addr);
            out = network::SocketAddr::FromSockaddr(reinterpret_cast<sockaddr*>(&address), sizeof(address));
        }
        else
        {
            sockaddr_in address{};
            address.sin_family = AF_INET;
            address.sin_port = htons(decoded.port);
            std::copy_n(decoded.ip.begin(), PACKETIA_STUN_IPV4_ADDRESS_BYTES, reinterpret_cast<uint8_t*>(&address.sin_addr));
            out = network::SocketAddr::FromSockaddr(reinterpret_cast<sockaddr*>(&address), sizeof(address));
        }
        return PeerAllowed(out, msg.method != StunMethod::CreatePermission) ? 0 : PACKETIA_STUN_ERROR_FORBIDDEN;
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
            {0, 0, uint8_t(code / PACKETIA_STUN_ERROR_CLASS_DIVISOR), uint8_t(code % PACKETIA_STUN_ERROR_CLASS_DIVISOR)}}};
        if (!unknown.empty())
        {
            StunAttribute attr{static_cast<uint16_t>(AttrType::UNKNOWN_ATTRIBUTES), {}};
            for (auto value : unknown)
            {
                attr.value.push_back(uint8_t(value >> CHAR_BIT));
                attr.value.push_back(uint8_t(value));
            }
            attributes.push_back(std::move(attr));
        }
        std::vector<uint8_t> response;
        StunCodec::BuildMessage(msg.method, StunClass::ErrorResponse, msg.txid, attributes, response, key, true);
        return response;
    }

    std::shared_ptr<network::UdpServer> OpenRelay(IpFamily family);
    void SendResponse(const network::SocketAddr& client, const std::vector<uint8_t>& response,
                      bool unauthorized)
    {
        if (unauthorized)
        {
            ++stats.unauthorized_401_requests;
            if (options.unauthorized_ratelimit)
            {
                const auto decision = unauthorized_limiter.Consume(
                    client, clock(), options.unauthorized_ratelimit_rps);
                if (decision.collision) ++stats.ratelimit_hash_collisions;
                if (decision.first_collision)
                    LOG_INFO("TURN 401 rate-limit bucket collision; sharing active budget");
                if (decision.first_drop)
                    LOG_INFO("TURN 401 rate-limit exceeded; suppressing responses for this window");
                if (decision.suppressed)
                {
                    ++stats.unauthorized_401_dropped;
                    return;
                }
            }
        }
        if (control->SendTo(client, response.data(), response.size()) && unauthorized)
            ++stats.unauthorized_401_responses;
    }
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
        return Error(msg, msg.method == StunMethod::Allocate
            ? PACKETIA_STUN_ERROR_ALLOCATION_MISMATCH : PACKETIA_STUN_ERROR_WRONG_CREDENTIALS, key);
    const auto unknown = UnknownAttributes(msg);
    if (!unknown.empty()) return Error(msg, PACKETIA_STUN_ERROR_UNKNOWN_ATTRIBUTE, key, unknown);
    const uint64_t now = clock();
    if (msg.method == StunMethod::Allocate || msg.method == StunMethod::Refresh)
    {
        uint32_t lifetime = kDefaultLifetime;
        if (AttributeCount(msg, AttrType::LIFETIME) > 1) return Error(msg, PACKETIA_STUN_ERROR_BAD_REQUEST, key);
        if (AttributeCount(msg, AttrType::LIFETIME) && !TurnCodec::DecodeUInt32(msg, AttrType::LIFETIME, lifetime))
            return Error(msg, PACKETIA_STUN_ERROR_BAD_REQUEST, key);
        if (msg.method == StunMethod::Refresh)
        {
            if (!allocation) return Error(msg, PACKETIA_STUN_ERROR_ALLOCATION_MISMATCH, key);
            if (lifetime != 0) lifetime = std::clamp(lifetime, kDefaultLifetime, kMaxLifetime);
            auto response = Success(msg, key, {TurnCodec::UInt32Attribute(AttrType::LIFETIME, lifetime)});
            if (response.empty()) return {};
            if (lifetime == 0) session.allocation.reset();
            else allocation->expires = Deadline(now, uint64_t(lifetime) * PACKETIA_TURN_MS_PER_SECOND);
            return response;
        }
        uint8_t transport = 0;
        if (AttributeCount(msg, AttrType::REQUESTED_TRANSPORT) != 1 ||
            !TurnCodec::DecodeRequestedTransport(msg, transport)) return Error(msg, PACKETIA_STUN_ERROR_BAD_REQUEST, key);
        if (transport != TurnCodec::kUdpTransport) return Error(msg, PACKETIA_STUN_ERROR_UNSUPPORTED_TRANSPORT_PROTOCOL, key);
        IpFamily requested_family = IpFamily::IPv4;
        if (AttributeCount(msg, AttrType::REQUESTED_ADDRESS_FAMILY) > 1) return Error(msg, PACKETIA_STUN_ERROR_BAD_REQUEST, key);
        if (const auto* family = msg.FindAttr(static_cast<uint16_t>(AttrType::REQUESTED_ADDRESS_FAMILY)))
        {
            if (family->len != PACKETIA_STUN_UINT32_ATTRIBUTE_BYTES) return Error(msg, PACKETIA_STUN_ERROR_BAD_REQUEST, key);
            const auto value = msg.raw[family->value_offset];
            if (value != PACKETIA_STUN_ADDRESS_FAMILY_IPV4 && value != PACKETIA_STUN_ADDRESS_FAMILY_IPV6)
                return Error(msg, PACKETIA_STUN_ERROR_BAD_REQUEST, key);
            requested_family = value == PACKETIA_STUN_ADDRESS_FAMILY_IPV6 ? IpFamily::IPv6 : IpFamily::IPv4;
        }
        const auto& public_ip = requested_family == IpFamily::IPv6 ? options.advertised_ip_v6 : options.advertised_ip;
        if (public_ip.empty()) return Error(msg, PACKETIA_STUN_ERROR_ADDRESS_FAMILY_NOT_SUPPORTED, key);
        lifetime = std::clamp(lifetime, kDefaultLifetime, kMaxLifetime);
        std::unique_ptr<Allocation> pending;
        if (allocation)
        {
            if (allocation->transaction != msg.txid) return Error(msg, PACKETIA_STUN_ERROR_ALLOCATION_MISMATCH, key);
            if (allocation->request != std::vector<uint8_t>(data, data + len)) return Error(msg, PACKETIA_STUN_ERROR_BAD_REQUEST, key);
            lifetime = static_cast<uint32_t>((allocation->expires - now +
                (PACKETIA_TURN_MS_PER_SECOND - 1)) / PACKETIA_TURN_MS_PER_SECOND);
        }
        else
        {
            if (AllocationCount() >= options.max_allocations ||
                AllocationCount(authenticated.username) >= options.max_allocations_per_user)
                return Error(msg, PACKETIA_STUN_ERROR_ALLOCATION_QUOTA_REACHED, key);
            pending = std::make_unique<Allocation>();
            pending->relay = OpenRelay(requested_family);
            if (!pending->relay) return Error(msg, PACKETIA_STUN_ERROR_INSUFFICIENT_CAPACITY, key);
            pending->advertised = network::SocketAddr::FromIPPort(public_ip, pending->relay->LocalAddress().Port());
            pending->username = authenticated.username;
            pending->transaction = msg.txid;
            pending->request.assign(data, data + len);
            pending->expires = Deadline(now, uint64_t(lifetime) * PACKETIA_TURN_MS_PER_SECOND);
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
    if (!allocation) return Error(msg, PACKETIA_STUN_ERROR_ALLOCATION_MISMATCH, key);

    std::vector<network::SocketAddr> peers;
    for (const auto& attr : msg.attrs)
    {
        if (attr.type == static_cast<uint16_t>(AttrType::MESSAGE_INTEGRITY)) break;
        if (attr.type != static_cast<uint16_t>(AttrType::XOR_PEER_ADDRESS)) continue;
        network::SocketAddr peer;
        if (const auto error = DecodePeer(msg, attr, *allocation, peer)) return Error(msg, error, key);
        peers.push_back(peer);
    }
    if (peers.empty()) return Error(msg, PACKETIA_STUN_ERROR_BAD_REQUEST, key);
    if (msg.method == StunMethod::ChannelBind && peers.size() != 1) return Error(msg, PACKETIA_STUN_ERROR_BAD_REQUEST, key);
    auto permissions = allocation->permissions;
    for (const auto& peer : peers) permissions[PeerIp(peer)] = Deadline(now, kPermissionMs);
    if (permissions.size() > options.max_permissions) return Error(msg, PACKETIA_STUN_ERROR_INSUFFICIENT_CAPACITY, key);
    if (msg.method == StunMethod::ChannelBind)
    {
        uint16_t channel = 0;
        if (AttributeCount(msg, AttrType::CHANNEL_NUMBER) != 1 || !TurnCodec::DecodeChannelNumber(msg, channel))
            return Error(msg, PACKETIA_STUN_ERROR_BAD_REQUEST, key);
        const auto existing = allocation->channels.find(channel);
        if (existing != allocation->channels.end() && !(existing->second.peer == peers.front())) return Error(msg, PACKETIA_STUN_ERROR_BAD_REQUEST, key);
        for (const auto& binding : allocation->channels)
            if (binding.first != channel && binding.second.peer == peers.front()) return Error(msg, PACKETIA_STUN_ERROR_BAD_REQUEST, key);
        auto channels = allocation->channels;
        channels[channel] = {peers.front(), Deadline(now, kChannelMs)};
        if (channels.size() > options.max_channels) return Error(msg, PACKETIA_STUN_ERROR_INSUFFICIENT_CAPACITY, key);
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
    if (!started || !data || len == 0 || len > PACKETIA_TURN_MAX_UDP_DATAGRAM_BYTES || !client.IsValid()) return;
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
        if (!unknown.empty()) response = Error(msg, PACKETIA_STUN_ERROR_UNKNOWN_ATTRIBUTE, {}, unknown);
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
            SendResponse(client, cached.response, cached.unauthorized);
        return;
    }
    auto authenticated = auth.Authenticate(data, len, session.auth);
    std::vector<uint8_t> response;
    if (authenticated.status == TurnAuthResult::Status::Response) response = std::move(authenticated.response);
    else if (authenticated.status == TurnAuthResult::Status::Authorized)
        response = HandleRequest(client, session, msg, authenticated, data, len);
    if (response.empty()) return;
    const bool unauthorized = authenticated.status == TurnAuthResult::Status::Response &&
        authenticated.error_code == PACKETIA_STUN_ERROR_UNAUTHORIZED;
    session.cache.push_back({msg.txid, {data, data + len}, std::move(response), Deadline(now, kCacheMs), unauthorized});
    if (session.cache.size() > kMaxCachedResponses) session.cache.pop_front();
    const auto& reply = session.cache.back().response;
    SendResponse(client, reply, unauthorized);
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
    std::array<uint8_t, PACKETIA_STUN_TRANSACTION_ID_BYTES> txid{};
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
            !state.options.max_permissions || !state.options.max_channels ||
            !state.options.unauthorized_ratelimit_rps) return;
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
        }, PACKETIA_TURN_PRUNE_INTERVAL_MS);
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
        state.unauthorized_limiter.Reset();
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
        stats.ratelimit_total_buckets = TurnAuthRateLimiter::kBucketCount;
        stats.ratelimit_occupied_buckets = impl_->unauthorized_limiter.OccupiedBuckets(impl_->clock());
    });
    return stats;
}

} // namespace protocol
