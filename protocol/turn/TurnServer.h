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
    std::string listen_ip = "127.0.0.1";
    uint16_t listen_port = 3478;
    // IPv6 wildcard listener with IPv4-mapped receive addresses normalized to IPv4.
    bool dual_stack = false;
    std::string relay_bind_ip = "127.0.0.1";
    std::string advertised_ip = "127.0.0.1";
    // Empty pairs disable the corresponding relay family.
    std::string relay_bind_ip_v6;
    std::string advertised_ip_v6;
    std::string realm = "packetia";
    uint16_t relay_port_min = 49152;
    uint16_t relay_port_max = 65535;
    size_t max_sessions = 512;
    size_t max_allocations = 128;
    size_t max_allocations_per_user = 8;
    size_t max_permissions = 64;
    size_t max_channels = 64;
    TurnAuth::Clock clock;
    // Default policy rejects private, loopback, link-local, and multicast peers.
    // A custom policy is checked on both control and forwarding paths.
    std::function<bool(const network::SocketAddr&)> allow_peer;
};

// IPv4/IPv6 UDP control and relaying. State and sockets are owned by one
// scheduler; public lifecycle/query methods synchronously dispatch to it.
class TurnServer : public std::enable_shared_from_this<TurnServer>
{
public:
    struct Stats
    {
        size_t sessions = 0;
        size_t allocations = 0;
        uint64_t client_to_peer = 0;
        uint64_t peer_to_client = 0;
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
