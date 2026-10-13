#ifndef PACKETIA_NETWORK_HTTP_OPTIONS_H_
#define PACKETIA_NETWORK_HTTP_OPTIONS_H_

#include <cstddef>
#include <cstdint>
#include <string>

namespace network::http
{

struct HttpOptions
{
    bool enabled = false;
    std::string listen_ip = "127.0.0.1";
    std::uint16_t listen_port = 8081;
    std::size_t max_body_bytes = 8192;
};

}

#endif // PACKETIA_NETWORK_HTTP_OPTIONS_H_
