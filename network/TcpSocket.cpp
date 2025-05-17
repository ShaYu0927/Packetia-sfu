//
// Created by roots on 2024/9/10.
//

#include "TcpSocket.h"


TcpSocket::TcpSocket(int m_socket)
    : m_socket_(m_socket)
{

}

TcpSocket::~TcpSocket()
{

}

int TcpSocket::Create()
{
    m_socket_ = socket(AF_INET, SOCK_STREAM, 0);
    return m_socket_;
}

bool TcpSocket::Bind(std::string ip, uint16_t port)
{
    struct sockaddr_in addr;
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
    struct sockaddr_in addr;
    socklen_t len = sizeof(addr);
    int client_fd = accept(m_socket_, (struct sockaddr*)&addr, &len);
    if(client_fd < 0)
    {
        perror("accept error");
        return -1;
    }
    return client_fd;
}

bool TcpSocket::Connect(std::string ip, uint16_t port, int timeout)
{
    if (timeout == 0)
    {
        struct sockaddr_in addr;
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
    close(m_socket_);
    m_socket_ = -1;
}

void TcpSocket::ShutdownWrite()
{
    shutdown(m_socket_, SHUT_WR);
    m_socket_ = 0;
}
