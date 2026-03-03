#ifndef _ENDPOINT_H_
#define _ENDPOINT_H_

#include <array>
#include <cstdint>
#include <string>

namespace net 
{
enum class IPFamily 
{
    IPv4,
    IPv6
};

class Endpoint
{
public:
    Endpoint() = default;

    static Endpoint FromIPv4(uint32_t ip_be, uint16_t port);
    static Endpoint FromIPv6(const std::array<uint8_t,16>& ip, uint16_t port);

    IPFamily Family() const { return family_; }
    uint16_t Port() const { return port_; }

    std::string ToString() const;

    bool operator==(const Endpoint& rhs) const;
    bool operator!=(const Endpoint& rhs) const { return !(*this == rhs); }


private:
    IPFamily family_{IPFamily::IPv4};
    std::array<uint8_t,16> addr_{};
    uint16_t port_{0};

};
};


#endif /* _ENDPOINT_H_ */