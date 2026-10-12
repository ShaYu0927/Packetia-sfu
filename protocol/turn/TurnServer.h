#ifndef PACKETIA_TURN_SERVER_H
#define PACKETIA_TURN_SERVER_H

#include "TurnAuth.h"
#include "UdpSocket.h"

#include <memory>

class TaskScheduler;

namespace protocol
{

struct TurnServerOptions
{
    std::string listen_ip = "127.0.0.1";         // 服务监听地址
    uint16_t listen_port = PACKETIA_TURN_DEFAULT_LISTEN_PORT;                 // 服务监听端口
    bool dual_stack = false;                     // 启用 IPv4 / IPv6 双栈

    std::string relay_bind_ip = "127.0.0.1";   // IPv4 中继绑定地址
    std::string advertised_ip = "127.0.0.1";   // 对外公布的 IPv4 中继地址
    std::string relay_bind_ip_v6;              // IPv6 中继绑定地址
    std::string advertised_ip_v6;              // 对外公布的 IPv6 中继地址

    std::string realm = "packetia";           // 认证域
    uint16_t relay_port_min = PACKETIA_TURN_DEFAULT_RELAY_PORT_MIN;          // 中继端口下限
    uint16_t relay_port_max = PACKETIA_TURN_DEFAULT_RELAY_PORT_MAX;          // 中继端口上限

    size_t max_sessions = PACKETIA_TURN_DEFAULT_MAX_SESSIONS;                // 最大会话数
    size_t max_allocations = PACKETIA_TURN_DEFAULT_MAX_ALLOCATIONS;             // 最大中继分配数
    size_t max_allocations_per_user = PACKETIA_TURN_DEFAULT_MAX_ALLOCATIONS_PER_USER;      // 单用户最大中继分配数
    size_t max_permissions = PACKETIA_TURN_DEFAULT_MAX_PERMISSIONS;              // Permission 数量上限
    size_t max_channels = PACKETIA_TURN_DEFAULT_MAX_CHANNELS;                 // Channel 数量上限

    bool unauthorized_ratelimit = false;      // UDP 401 反射响应限流，默认关闭
    uint32_t unauthorized_ratelimit_rps = PACKETIA_TURN_DEFAULT_UNAUTHORIZED_RPS;  // 每来源 IP、每秒的响应预算

    TurnAuth::Clock clock;                    // 认证时钟

    // 对端准入检查：true 允许，false 拒绝
    std::function<bool(const network::SocketAddr&)> allow_peer;
};


class TurnServer : public std::enable_shared_from_this<TurnServer>
{
public:
    struct Stats
    {
        size_t sessions = 0;
        size_t allocations = 0;
        uint64_t client_to_peer = 0;
        uint64_t peer_to_client = 0;
        uint64_t unauthorized_401_requests = 0;
        uint64_t unauthorized_401_responses = 0; // Successfully accepted by UDP socket.
        uint64_t unauthorized_401_dropped = 0;   // Suppressed by rate limit only.
        uint64_t ratelimit_hash_collisions = 0;
        size_t ratelimit_occupied_buckets = 0;
        size_t ratelimit_total_buckets = 0;
    };

    TurnServer(std::shared_ptr<TaskScheduler> scheduler, TurnServerOptions options,
               TurnAuth::PasswordLookup lookup);
    ~TurnServer();
    bool Start();
    void Stop();
    void Tick();
    network::SocketAddr LocalAddress() const;
    Stats GetStats() const;

private:
    class ControlHandler;
    class RelayHandler;
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace protocol

#endif
