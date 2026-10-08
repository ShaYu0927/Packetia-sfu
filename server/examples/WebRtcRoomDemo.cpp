#include "WebRtcService.h"
#include "EventLoop.h"
#include "EndpointBase.h"
#include "WorkerRegistry.h"
#include "protocol/turn/TurnServer.h"

#include <charconv>
#include <csignal>
#include <cstdlib>
#include <iostream>

namespace
{
volatile std::sig_atomic_t stopping = 0;
void StopSignal(int) { stopping = 1; }
bool ParsePort(const char* text, uint16_t& port)
{
    const std::string_view value(text);
    unsigned number = 0;
    const auto result = std::from_chars(value.data(), value.data() + value.size(), number);
    if (result.ec != std::errc{} || result.ptr != value.data() + value.size() || !number || number > 65535) return false;
    port = static_cast<uint16_t>(number);
    return true;
}
}

int main(int argc, char** argv)
{
    if (argc != 4) { std::cerr << "Usage: PacketiaRoomDemo WS_PORT MEDIA_PORT TURN_PORT\n"; return 2; }
    config::AppConfig config;
    protocol::TurnServerOptions turnOptions;
    config.listen_ip = config.webrtc.public_ip = "127.0.0.1";
    config.webrtc.enabled = true;
    config.webrtc.max_sessions = 16;
    if (!ParsePort(argv[1], config.websocket_port) || !ParsePort(argv[2], config.udp_port) ||
        !ParsePort(argv[3], turnOptions.listen_port)) return 2;
    const auto token = std::getenv("PACKETIA_WEBRTC_TOKEN");
    const auto user = std::getenv("PACKETIA_TURN_USER");
    const auto password = std::getenv("PACKETIA_TURN_PASSWORD");
    if (!token || !*token || !user || !*user || !password || !*password) return 2;
    config.webrtc.token = token;
    turnOptions.relay_port_min = turnOptions.relay_port_max = 0;
    // The local demo shares one account across participants and interfaces.
    turnOptions.max_allocations_per_user = config.webrtc.max_sessions * 4;
    turnOptions.allow_peer = [mediaPort = config.udp_port](const network::SocketAddr& peer) {
        return peer == network::SocketAddr::FromIPPort("127.0.0.1", mediaPort);
    };
    EventLoop loop(1);
    if (!loop.Start()) return 1;
    WorkerRegistry workers;
    workers.Add({POOL_MEDIA, 2, 4096, ShardedWorkerPool::DropPolicy::DropHead},
        std::make_shared<utils::EndpointJobHandler>(&utils::EndpointManager::Instance()));
    if (workers.Start() != 0) return 1;
    auto turn = std::make_shared<protocol::TurnServer>(loop.GetTaskScheduler(), turnOptions,
        [username = std::string(user), secret = std::string(password)](std::string_view name, std::string& out) {
            if (name != username) return false;
            out = secret;
            return true;
        });
    auto rtc = std::make_shared<server::WebRtcService>(&loop, config, nullptr);
    if (!turn->Start() || !rtc->Start()) return 1;
    std::signal(SIGINT, StopSignal);
    std::signal(SIGTERM, StopSignal);
    std::cout << "ROOM_DEMO_READY" << std::endl;
    while (!stopping) std::this_thread::sleep_for(std::chrono::milliseconds(100));
    rtc->Stop();
    turn->Stop();
    workers.Stop();
    loop.Stop();
    return 0;
}
