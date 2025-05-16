//
// Created by roots on 2024/9/17.
//

#include "SocketUtil.h"
#include <fcntl.h>
bool SocketUtil::Bind(int sockfd, std::string ip, uint16_t port)
{
    struct sockaddr_in addr = {0};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    addr.sin_addr.s_addr = inet_addr(ip.c_str());
    if(::bind(sockfd,(struct sockaddr*)&addr, sizeof addr) == -1)
    {
        return false;
    }
    return true;
}
void SocketUtil::SetNonBlock(int fd)
{
     int flags = fcntl(fd, F_GETFL, 0);
    if (flags == -1)
        flags = 0;
    fcntl(fd, F_SETFL, flags | O_NONBLOCK);
}
void SocketUtil::SetBlock(int fd, int write_timeout)
{
    //设置套接字 fd 为非阻塞模式。非阻塞模式意味着读写操作不会阻塞当前线程，如果没有数据可用，操作会立即返回，而不是等待
    int flags = fcntl(fd, F_GETFL, 0);
    fcntl(fd, F_SETFL, flags&(~O_NONBLOCK));

    if(write_timeout > 0)
    {
        struct timeval tv = {write_timeout/1000, (write_timeout%1000)*1000};
        setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, (char *)&tv, sizeof(unsigned long));

    }
}
void SocketUtil::SetReuseAddr(int fd)
{
    int on = 1;
    setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, (char *)&on, sizeof(on));
}
void SocketUtil::SetReusePort(int sockfd)
{
#ifdef SO_REUSEPORT
    int on = 1;
    setsockopt(sockfd, SOL_SOCKET, SO_REUSEPORT, (const char*)&on, sizeof(on));
#endif
}
//禁用 Nagle 算法
void SocketUtil::SetNoDelay(int sockfd)
{
#ifdef TCP_NODELAY
    int on = 1;
    int ret = setsockopt(sockfd, IPPROTO_TCP, TCP_NODELAY, (char *)&on, sizeof(on));
#endif
}
//设置保持活动选项
void SocketUtil::SetKeepAlive(int sockfd)
{
    int on = 1;
    setsockopt(sockfd, SOL_SOCKET, SO_KEEPALIVE, (char *)&on, sizeof(on));
}

void SocketUtil::SetNoSigpipe(int sockfd)
{
#ifdef SO_NOSIGPIPE
    int on = 1;
    setsockopt(sockfd, SOL_SOCKET, SO_NOSIGPIPE, (char *)&on, sizeof(on)); //setsockopt可以控制套接字的行为
#endif
}
void SocketUtil::SetSendBufSize(int sockfd, int size)
{
    setsockopt(sockfd, SOL_SOCKET, SO_SNDBUF, (char *)&size, sizeof(size));
}
void SocketUtil::SetRecvBufSize(int sockfd, int size)
{
    setsockopt(sockfd, SOL_SOCKET, SO_RCVBUF, (char *)&size, sizeof(size));
}
std::string SocketUtil::GetPeerIp(int sockfd)
{
    struct sockaddr_in addr = { 0 };
    socklen_t addrlen = sizeof(struct sockaddr_in);
    if (getpeername(sockfd, (struct sockaddr *)&addr, &addrlen) == 0)
    {
        return inet_ntoa(addr.sin_addr);
    }
    return "0.0.0.0";
}

int SocketUtil::GetSocketAddr(int sockfd, struct sockaddr_in* addr)
{
    socklen_t addrlen = sizeof(struct sockaddr_in);
    return getsockname(sockfd, (struct sockaddr*)addr, &addrlen);
}

std::string SocketUtil::GetSocketIp(int sockfd)
{
    struct sockaddr_in addr = {0};
    char str[INET_ADDRSTRLEN] = "127.0.0.1";
    if (GetSocketAddr(sockfd, &addr) == 0) {
        inet_ntop(AF_INET, &addr.sin_addr, str, sizeof(str));
    }
    return str;
}

uint16_t SocketUtil::GetPeerPort(int sockfd)
{
    struct sockaddr_in addr = { 0 };
    socklen_t addrlen = sizeof(struct sockaddr_in);
    if (getpeername(sockfd, (struct sockaddr *)&addr, &addrlen) == 0)
    {
        return ntohs(addr.sin_port);
    }
    return 0;
}
int SocketUtil::GetPeerAddr(int sockfd, struct sockaddr_in *addr)
{
    socklen_t addrlen = sizeof(struct sockaddr_in);
    return getpeername(sockfd, (struct sockaddr *)addr, &addrlen);
}
void SocketUtil::Close(int sockfd)
{
    close(sockfd);
}
bool SocketUtil::Connect(int sockfd, std::string ip, uint16_t port, int timeout)
{
    bool isConnected = true;
    if(timeout > 0)
    {
        SocketUtil::SetNonBlock(sockfd);
    }
    struct sockaddr_in addr = { 0 };
    socklen_t addrlen = sizeof(addr);
	addr.sin_family = AF_INET;
	addr.sin_port = htons(port);
	addr.sin_addr.s_addr = inet_addr(ip.c_str());

    if(connect(sockfd,(struct sockaddr*)&addr, addrlen))
    {
        if(timeout > 0)
        {
            isConnected = false;
            fd_set fd_write;
            FD_ZERO(&fd_write);
			FD_SET(sockfd, &fd_write);
            struct timeval tv = { timeout / 1000, timeout % 1000 * 1000 };
			select((int)sockfd + 1, NULL, &fd_write, NULL, &tv);
            if (FD_ISSET(sockfd, &fd_write)) {
                isConnected = true;
			}
			SetBlock(sockfd);
        }
        else
        {
            isConnected = false;
        }
    }
    return isConnected;
}