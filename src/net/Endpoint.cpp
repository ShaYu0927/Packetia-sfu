#include "Endpoint.h"
#include <arpa/inet.h>

namespace net
{
std::array<uint8_t,4> Endpoint::IpBytesV4() const
{
    std::array<uint8_t,4> out{};
    if (!IsV4()) return out;
    out[0] = addr_[0]; out[1] = addr_[1]; out[2] = addr_[2]; out[3] = addr_[3];
    return out;
}

Endpoint Endpoint::FromIPv4(uint32_t ip_be, uint16_t port)
{
    Endpoint ep;
    ep.family_ = IPFamily::IPv4;
    ep.port_ = port;
    ep.addr_.fill(0);
    ep.addr_[0] = uint8_t((ip_be >> 24) & 0xFF);
    ep.addr_[1] = uint8_t((ip_be >> 16) & 0xFF);
    ep.addr_[2] = uint8_t((ip_be >> 8) & 0xFF);
    ep.addr_[3] = uint8_t(ip_be & 0xFF);
    return ep;
}

Endpoint Endpoint::FromIPv6(const std::array<uint8_t,16>& ip, uint16_t port)
{
    Endpoint ep;
    ep.family_ = IPFamily::IPv6;
    ep.port_ = port;
    ep.addr_ = ip;
    return ep;
}

std::string Endpoint::IpToString() const 
{
    char buf[INET6_ADDRSTRLEN] = {0};
    if (IsV4()) 
    {
        uint8_t b0 = addr_[0], b1 = addr_[1], b2 = addr_[2], b3 = addr_[3];
        std::snprintf(buf, sizeof(buf), "%u.%u.%u.%u", b0, b1, b2, b3);
        return std::string(buf);
    } 
    else 
    {
        if (!inet_ntop(AF_INET6, addr_.data(), buf, sizeof(buf))) return {};
        return std::string(buf);
    }
}

std::string Endpoint::ToString() const
{
    if (IsV4()) 
    {
        return IpToString() + ":" + std::to_string(port_);
    }
    return "[" + IpToString() + "]:" + std::to_string(port_);
}

}