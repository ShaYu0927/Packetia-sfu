#include "UdpSocket.h"

int network::UdpSocket::Create(int family, bool dual_stack)
{
    if ((family != AF_INET && family != AF_INET6) || (dual_stack && family != AF_INET6)) return -1;
    family_ = family;
    dual_stack_ = dual_stack;
    fd_ = ::socket(family, SOCK_DGRAM, 0);
    if (fd_ >= 0 && family == AF_INET6)
    {
        int only_v6 = dual_stack ? 0 : 1;
        if (setsockopt(fd_, IPPROTO_IPV6, IPV6_V6ONLY, &only_v6, sizeof(only_v6)) != 0)
        {
            Close();
            return -1;
        }
    }
    return fd_;
}

bool network::UdpSocket::Bind(const std::string &ip, uint16_t port, bool reuse_address)
{
    const auto addr = SocketAddr::FromIPPort(ip.empty() ? (family_ == AF_INET6 ? "::" : "0.0.0.0") : ip, port);
    if (!addr.IsValid() || addr.ss.ss_family != family_) return false;

    int reuse = reuse_address ? 1 : 0;
    setsockopt(fd_, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));

    int ret = ::bind(fd_, reinterpret_cast<const sockaddr*>(&addr.ss), addr.len);

    if (ret < 0) 
    {
        perror("udp bind error");
        return false;
    }

    SocketUtil::SetNonBlock(fd_);
    return true;
}

void network::UdpSocket::Close()
{
    if (fd_ >= 0) 
    {
        ::close(fd_);
        fd_ = -1;
    }
}

int network::UdpSocket::RecvFrom(uint8_t *buf, size_t cap, SocketAddr &src)
{
    sockaddr_storage ss{};
    socklen_t slen = sizeof(ss);
    int n;
    do { n = ::recvfrom(fd_, buf, cap, 0, (sockaddr*)&ss, &slen); }
    while (n < 0 && errno == EINTR);
    if (n < 0) 
    {
        if (errno == EAGAIN || errno == EWOULDBLOCK) return -1;
        return -2; 
    }
    src = SocketAddr::FromSockaddr((sockaddr*)&ss, slen).Normalized();
    return n;
}

int network::UdpSocket::SendTo(const SocketAddr &dst, const uint8_t *data, size_t len)
{
    if (!dst.IsValid()) return -2;
    auto target = dst.Normalized();
    // Dual-stack sockets send IPv4 traffic using the kernel's mapped address form.
    if (family_ == AF_INET6 && target.IsV4() && dual_stack_)
    {
        sockaddr_in6 mapped{};
        mapped.sin6_family = AF_INET6;
        mapped.sin6_port = htons(target.Port());
        mapped.sin6_addr.s6_addr[10] = mapped.sin6_addr.s6_addr[11] = 0xff;
        memcpy(mapped.sin6_addr.s6_addr + 12, target.IPv4Bytes().data(), 4);
        target = SocketAddr::FromSockaddr(reinterpret_cast<const sockaddr*>(&mapped), sizeof(mapped));
    }
    if (target.ss.ss_family != family_) return -2;
    int n;
    do { n = ::sendto(fd_, data, len, 0, reinterpret_cast<const sockaddr*>(&target.ss), target.len); }
    while (n < 0 && errno == EINTR);
    if (n < 0)
    {
        if (errno == EAGAIN || errno == EWOULDBLOCK) return -1;
        if (errno == ENOBUFS) return -1;
        return -2;
    }
    return 0;
}
