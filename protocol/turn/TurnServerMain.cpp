#include "TurnServer.h"
#include "EventLoop.h"
#include "StringUtil.h"

#include <charconv>
#include <csignal>
#include <cstdlib>
#include <iostream>
#include <stdexcept>

namespace
{
volatile std::sig_atomic_t stopping = 0;
void StopSignal(int) { stopping = 1; }

uint16_t Port(std::string_view text)
{
    unsigned int value = 0;
    const auto result = std::from_chars(text.data(), text.data() + text.size(), value);
    if (result.ec != std::errc{} || result.ptr != text.data() + text.size() || value > 65535)
        throw std::invalid_argument("Invalid port");
    return static_cast<uint16_t>(value);
}

void Usage()
{
    std::cout << "PacketiaTurn [--listen-ip IP] [--listen-port PORT] [--relay-ip IP]\n"
                 "             [--public-ip IP] [--realm REALM]\n"
                 "             [--relay-min-port PORT] [--relay-max-port PORT] [--local-test]\n"
                 "Account: PACKETIA_TURN_USER and PACKETIA_TURN_PASSWORD environment variables.\n"
                 "--local-test permits only loopback peers. Default peer policy permits public IPv4.\n";
}
} // namespace

int main(int argc, char** argv)
{
    protocol::TurnServerOptions options;
    try
    {
        for (int i = 1; i < argc; ++i)
        {
            const std::string_view argument(argv[i]);
            if (argument == "--help") { Usage(); return 0; }
            if (argument == "--local-test")
            {
                options.allow_peer = [](const network::SocketAddr& peer) {
                    const auto bytes = peer.IPv4Bytes();
                    return bytes.size() == 4 && static_cast<unsigned char>(bytes[0]) == 127;
                };
                continue;
            }
            if (i + 1 == argc) throw std::invalid_argument("Missing argument value");
            const std::string_view value(argv[++i]);
            if (argument == "--listen-ip") options.listen_ip = value;
            else if (argument == "--listen-port") options.listen_port = Port(value);
            else if (argument == "--relay-ip") options.relay_bind_ip = value;
            else if (argument == "--public-ip") options.advertised_ip = value;
            else if (argument == "--realm") options.realm = value;
            else if (argument == "--relay-min-port") options.relay_port_min = Port(value);
            else if (argument == "--relay-max-port") options.relay_port_max = Port(value);
            else throw std::invalid_argument("Unknown argument");
        }
    }
    catch (const std::invalid_argument& error)
    {
        std::cerr << error.what() << '\n';
        Usage();
        return 2;
    }
    const char* configured_user = std::getenv("PACKETIA_TURN_USER");
    const char* configured_password = std::getenv("PACKETIA_TURN_PASSWORD");
    if (!configured_user || !configured_password)
    {
        std::cerr << "Set PACKETIA_TURN_USER and PACKETIA_TURN_PASSWORD.\n";
        return 2;
    }
    const std::string username(configured_user), password(configured_password);
    if (username.empty() || username.size() > 512 || password.empty() || password.size() > 763 ||
        !utils::IsPrintableAscii(username) || !utils::IsPrintableAscii(password))
    {
        std::cerr << "TURN account must use nonempty printable ASCII credentials within protocol limits.\n";
        return 2;
    }
    EventLoop loop{1};
    if (!loop.Start()) return 1;
    auto server = std::make_shared<protocol::TurnServer>(loop.GetTaskScheduler(), options,
        [username, password](std::string_view user, std::string& out) {
            if (user != username) return false;
            out = password;
            return true;
        });
    if (!server->Start())
    {
        std::cerr << "Could not start TURN UDP listener; check addresses, port range, and realm.\n";
        return 1;
    }
    std::signal(SIGINT, StopSignal);
    std::signal(SIGTERM, StopSignal);
    std::cout << "TURN UDP listening on " << server->LocalAddress().ToString() << std::endl;
    while (!stopping) std::this_thread::sleep_for(std::chrono::milliseconds(100));
    server->Stop();
    loop.Stop();
    return 0;
}
