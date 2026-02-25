#ifndef _UDPSOCKET_H_
#define _UDPSOCKET_H_

#include <string>
#include <cstdint>
#include "Socket.h"
#include "SocketUtil.h"

namespace network
{

struct SocketAddr 
{
    sockaddr_storage ss{};
    socklen_t len{0};

    static SocketAddr FromIPPort(const std::string& ip, uint16_t port)
    {
        SocketAddr a;
        sockaddr_in in{};
        in.sin_family = AF_INET;
        in.sin_port = htons(port);
        in.sin_addr.s_addr = inet_addr(ip.c_str());
        memcpy(&a.ss, &in, sizeof(in));
        a.len = sizeof(in);
        return a;
    }

    static SocketAddr FromSockaddr(const sockaddr* sa, socklen_t slen) 
    {
        SocketAddr a;
        memcpy(&a.ss, sa, slen);
        a.len = slen;
        return a;
    }

    std::string ToString() const 
    {
        char ip[64]{};
        uint16_t port = 0;
        if (ss.ss_family == AF_INET) 
        {
            auto* in = (sockaddr_in*)&ss;
            inet_ntop(AF_INET, &in->sin_addr, ip, sizeof(ip));
            port = ntohs(in->sin_port);
        } 
        else if (ss.ss_family == AF_INET6) 
        {
            auto* in6 = (sockaddr_in6*)&ss;
            inet_ntop(AF_INET6, &in6->sin6_addr, ip, sizeof(ip));
            port = ntohs(in6->sin6_port);
        }
        return std::string(ip) + ":" + std::to_string(port);
    }

    bool operator==(const SocketAddr& other) const
    {
        if (ss.ss_family != other.ss.ss_family) return false;

        if (ss.ss_family == AF_INET)
        {
            auto* a = (const sockaddr_in*)&ss;
            auto* b = (const sockaddr_in*)&other.ss;

            return a->sin_port == b->sin_port &&
                a->sin_addr.s_addr == b->sin_addr.s_addr;
        }
        else if (ss.ss_family == AF_INET6)
        {
            auto* a = (const sockaddr_in6*)&ss;
            auto* b = (const sockaddr_in6*)&other.ss;

            return a->sin6_port == b->sin6_port &&
                memcmp(&a->sin6_addr, &b->sin6_addr, sizeof(in6_addr)) == 0;
        }

        return false;
    }
};

struct SocketAddrHash
{
    size_t operator()(const SocketAddr& a) const noexcept
    {
        if (a.ss.ss_family == AF_INET)
        {
            auto* in = (const sockaddr_in*)&a.ss;

            uint64_t key =
                (uint64_t(in->sin_addr.s_addr) << 16) |
                uint64_t(in->sin_port);

            return std::hash<uint64_t>()(key);
        }
        else if (a.ss.ss_family == AF_INET6)
        {
            auto* in6 = (const sockaddr_in6*)&a.ss;

            const uint64_t* p =
                reinterpret_cast<const uint64_t*>(&in6->sin6_addr);

            uint64_t h =
                p[0] ^ p[1] ^ uint64_t(in6->sin6_port);

            return std::hash<uint64_t>()(h);
        }

        return 0;
    }
};


class UdpSocket 
{
public:
    explicit UdpSocket(int fd = -1) : fd_(fd) {}
    ~UdpSocket() = default;

    int  Create(); // socket(AF_INET, SOCK_DGRAM, 0)
    bool Bind(const std::string& ip, uint16_t port);
    void Close();

    int Fd() const { return fd_; }

    // recvfrom / sendto
    int RecvFrom(uint8_t* buf, size_t cap, SocketAddr& src);
    int SendTo(const SocketAddr& dst, const uint8_t* data, size_t len);

private:
    int fd_{-1};
};

}

#endif /* _UDPSOCKET_H_ */