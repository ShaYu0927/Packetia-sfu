//
// Created by roots on 2024/9/10.
//

#include "TcpSocket.h"
#include "SocketUtil.h"
#include "logger.h"
#include "Socket.h"

TcpSocket::TcpSocket(int m_socket)
    : m_socket_(m_socket)
{

}

TcpSocket::~TcpSocket()
{

}

int TcpSocket::Create()
{
    int fd;
    do { fd = socket(AF_INET, SOCK_STREAM, 0); }
    while (fd < 0 && errno == EINTR);
    if (fd < 0) return -1;
    if (!SocketUtil::SetCloseOnExec(fd) || !SocketUtil::SetNoSigpipe(fd))
    {
        const int error = errno;
        ::close(fd);
        errno = error;
        return -1;
    }
    Close();
    m_socket_ = fd;
    return m_socket_;
}

bool TcpSocket::Bind(std::string ip, uint16_t port)
{
    struct sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    addr.sin_addr.s_addr = inet_addr(ip.c_str());

    int ret = bind(m_socket_, (struct sockaddr*)&addr, sizeof(addr));
    if (ret < 0)
    {
        perror("bind error");
        return false;
    }
    return true;
}

bool TcpSocket::Listen(int backlog)
{
    int ret = listen(m_socket_, backlog);
    if (ret < 0)
    {
        perror("listen error");
        return false;
    }
    return true;
}

int TcpSocket::Accept()
{
    struct sockaddr_in addr{};
    socklen_t len = sizeof(addr);
    int fd;
    do
    {
#if defined(__linux) || defined(__linux__)
        fd = ::accept4(m_socket_, (sockaddr*)&addr, &len, SOCK_NONBLOCK | SOCK_CLOEXEC);
#else
        fd = ::accept(m_socket_, (sockaddr*)&addr, &len);
#endif
    }
    while (fd < 0 && errno == EINTR);
    if(fd < 0)
    {
        if (errno == EAGAIN || errno == EWOULDBLOCK) 
        {
            return -1;
        }
        const int error = errno;
        LOG_ERROR("accept failed errno=" + std::to_string(error));
        errno = error;
        return -1;
    }
#if !defined(__linux) && !defined(__linux__)
    if (!SocketUtil::SetNonBlock(fd) || !SocketUtil::SetCloseOnExec(fd))
    {
        const int error = errno;
        ::close(fd);
        errno = error;
        return -1;
    }
#endif
    if (!SocketUtil::SetNoSigpipe(fd))
    {
        const int error = errno;
        ::close(fd);
        errno = error;
        return -1;
    }
    return fd;
}

bool TcpSocket::Connect(std::string ip, uint16_t port, int timeout)
{
    if (timeout == 0)
    {
        struct sockaddr_in addr{};
        addr.sin_family = AF_INET;
        addr.sin_port = htons(port);
        addr.sin_addr.s_addr = inet_addr(ip.c_str());
        int ret = connect(m_socket_, (struct sockaddr*)&addr, sizeof(addr));
        if (ret < 0)
        {
            perror("connect error");
            return false;
        }
    }
    return false;
}

void TcpSocket::Close()
{
    if (m_socket_ >= 0) close(m_socket_);
    m_socket_ = -1;
}

void TcpSocket::ShutdownWrite()
{
    shutdown(m_socket_, SHUT_WR);
    m_socket_ = 0;
}
