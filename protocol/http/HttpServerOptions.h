#ifndef PACKETIA_PROTOCOL_HTTP_SERVER_OPTIONS_H_
#define PACKETIA_PROTOCOL_HTTP_SERVER_OPTIONS_H_

#include <cstddef>
#include <cstdint>
#include <string>

namespace protocol::http
{

struct HttpServerOptions
{
    bool enabled = false;
    std::string listen_ip = "127.0.0.1";
    std::uint16_t listen_port = 8081;
    std::size_t max_body_bytes = 8192;
    std::size_t max_connections = 1024;
    std::size_t max_header_bytes = 8192;
    std::size_t max_response_bytes = 1024 * 1024;
    std::size_t max_total_response_bytes = 16 * 1024 * 1024;
    std::uint32_t request_timeout_seconds = 15;
    std::uint32_t idle_timeout_seconds = 30;
    std::uint32_t service_interval_ms = 5;

    bool IsValid() const noexcept
    {
        return !listen_ip.empty() && listen_port && max_body_bytes &&
            max_body_bytes <= 1024 * 1024 && max_connections && max_connections <= 65536 &&
            max_header_bytes >= 1024 && max_header_bytes <= 65536 &&
            max_response_bytes && max_response_bytes <= 16 * 1024 * 1024 &&
            max_total_response_bytes >= max_response_bytes &&
            max_total_response_bytes <= 1024ULL * 1024 * 1024 &&
            request_timeout_seconds && request_timeout_seconds <= 300 &&
            idle_timeout_seconds && idle_timeout_seconds <= 3600 &&
            service_interval_ms && service_interval_ms <= 100;
    }
};

}

#endif // PACKETIA_PROTOCOL_HTTP_SERVER_OPTIONS_H_
