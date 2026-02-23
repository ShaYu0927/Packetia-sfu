#include "UdpSocket.h"

int network::UdpSocket::Create()
{
    fd_ = ::socket(AF_INET, SOCK_DGRAM, 0);
    return fd_;
}

bool network::UdpSocket::Bind(const std::string &ip, uint16_t port)
{
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    addr.sin_addr.s_addr = ip.empty() ? htonl(INADDR_ANY) : inet_addr(ip.c_str());

    int reuse = 1;
    setsockopt(fd_, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));

    int ret = ::bind(fd_, (sockaddr*)&addr, sizeof(addr));

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
    int n = ::recvfrom(fd_, buf, cap, 0, (sockaddr*)&ss, &slen);
    if (n < 0) 
    {
        if (errno == EAGAIN || errno == EWOULDBLOCK) return -1;
        return -2; 
    }
    src = SocketAddr::FromSockaddr((sockaddr*)&ss, slen);
    return n;
}

int network::UdpSocket::SendTo(const SocketAddr &dst, const uint8_t *data, size_t len)
{
    int n = ::sendto(fd_, data, len, 0, (sockaddr*)&dst.ss, dst.len);
    if (n < 0)
    {
        if (errno == EAGAIN || errno == EWOULDBLOCK) return -1;
        if (errno == ENOBUFS) return -1;
        return -2;
    }
    return 0;
}
