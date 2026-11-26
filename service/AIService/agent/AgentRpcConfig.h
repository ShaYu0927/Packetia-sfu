#pragma once

#include <charconv>
#include <cstdlib>
#include <stdexcept>
#include <string>
#include <string_view>

namespace service::ai::rpc_config
{
inline constexpr auto kDefaultAddress = "127.0.0.1:50051";
inline constexpr auto kTokenHeader = "x-packetia-agent-token";

inline void RequireLoopbackAddress(std::string_view address)
{
    constexpr std::string_view prefix = "127.0.0.1:";
    if (address.substr(0, prefix.size()) != prefix)
        throw std::invalid_argument("Agent RPC address must use 127.0.0.1:<port>");
    address.remove_prefix(prefix.size());
    unsigned port = 0;
    const auto end = address.data() + address.size();
    const auto parsed = std::from_chars(address.data(), end, port);
    if (parsed.ec != std::errc{} || parsed.ptr != end || port == 0 || port > 65535)
        throw std::invalid_argument("Invalid Agent RPC port");
}

inline std::string Token()
{
    const auto* value = std::getenv("PACKETIA_AGENT_RPC_TOKEN");
    const std::string token = value ? value : "";
    if (token.size() != 64 || token.find_first_not_of("0123456789abcdefABCDEF") != std::string::npos)
        throw std::invalid_argument("PACKETIA_AGENT_RPC_TOKEN must contain 64 hexadecimal characters");
    return token;
}
}
