#include "WebRtcService.h"
#include "TurnServer.h"
#include "EventLoop.h"
#include "EndpointBase.h"
#include "WorkerRegistry.h"

#include <charconv>
#include <chrono>
#include <csignal>
#include <cstdlib>
#include <iostream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <utility>

namespace
{
volatile std::sig_atomic_t stopping = 0;
void StopSignal(int) { stopping = 1; }
uint16_t Port(std::string_view text)
{
    unsigned value = 0;
    const auto result = std::from_chars(text.data(), text.data() + text.size(), value);
    if (result.ec != std::errc{} || result.ptr != text.data() + text.size() || !value || value > 65535)
        throw std::invalid_argument("Invalid demo port");
    return static_cast<uint16_t>(value);
}
std::string Credential(const char* name, std::size_t minimum = 1)
{
    const auto value = std::getenv(name);
    if (!value || std::string_view(value).size() < minimum) throw std::invalid_argument("Missing demo credential");
    return value;
}
struct Demo
{
    EventLoop loop{3};
    WorkerRegistry workers;
    std::shared_ptr<server::WebRtcService> rtc;
    std::shared_ptr<protocol::TurnServer> turn;
    protocol::TurnServerOptions turn_options;
    std::string turn_user, turn_password;
    Demo(uint16_t ws, uint16_t media, uint16_t relay)
    {
        config::AppConfig config;
        config.listen_ip = config.webrtc.public_ip = "127.0.0.1";
        config.websocket_port = ws; config.udp_port = media;
        config.webrtc.enabled = true;
        config.webrtc.api_keys.emplace(Credential("PACKETIA_WEBRTC_API_KEY"), Credential("PACKETIA_WEBRTC_API_SECRET", 32));
        rtc = std::make_shared<server::WebRtcService>(&loop, config, nullptr);
        turn_options.listen_port = relay;
        turn_options.relay_port_min = turn_options.relay_port_max = 0;
        // This local demo relays only to its SFU socket.
        turn_options.allow_peer = [media](const network::SocketAddr& peer) {
            return peer.IPv4Bytes() == std::string("\x7f\x00\x00\x01", 4) && peer.Port() == media;
        };
        turn_user = Credential("PACKETIA_TURN_USER");
        turn_password = Credential("PACKETIA_TURN_PASSWORD");
        workers.Add({POOL_MEDIA, 4, 4096, ShardedWorkerPool::DropPolicy::DropHead},
            std::make_shared<utils::EndpointJobHandler>(&utils::EndpointManager::Instance()));
    }
    ~Demo()
    {
        rtc->Stop();
        if (turn) turn->Stop();
        workers.Stop(); loop.Stop();
    }
    bool Start()
    {
        if (!loop.Start() || workers.Start() != 0) return false;
        turn = std::make_shared<protocol::TurnServer>(loop.GetTaskScheduler(), std::move(turn_options),
            [user = turn_user, password = turn_password](std::string_view candidate, std::string& out) {
                if (candidate != user) return false;
                out = password; return true;
            });
        return turn->Start() && rtc->Start();
    }
};
}
int main(int argc, char** argv)
{
    if (argc != 4) { std::cerr << "Usage: PacketiaRoomDemo WS_PORT MEDIA_PORT TURN_PORT\n"; return 2; }
    std::signal(SIGINT, StopSignal); std::signal(SIGTERM, StopSignal);
    try
    {
        Demo demo(Port(argv[1]), Port(argv[2]), Port(argv[3]));
        if (!demo.Start()) { std::cerr << "Room demo startup failed\n"; return 1; }
        std::cout << "ROOM_DEMO_READY" << std::endl;
        while (!stopping) std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
    catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
}
