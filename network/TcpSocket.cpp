//
// Created by roots on 2024/9/10.
//

#include "TcpSocket.h"


TcpSocket::TcpSocket(int m_socket)
:m_socket_(m_socket)
{

}
TcpSocket::~TcpSocket()
{

}

int    TcpSocket::Create()
{
    m_socket_ = socket(AF_INET, SOCK_STREAM, 0);
    return m_socket_;
}
bool   TcpSocket::Bind(std::string ip, uint16_t port)
{
    struct sockaddr_in addr;
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    addr.sin_addr.s_addr = inet_addr(ip.c_str());
    if (bind(m_socket_, (struct sockaddr*)&addr, sizeof(addr)) < 0)
    {
        return false;
    }
    return true;
}
bool   TcpSocket::Listen(int backlog)
{
    if (listen(m_socket_, backlog) < 0)
    {
        return false;
    }
    return true;
}
int    TcpSocket::Accept()
{
    struct sockaddr_in addr;
    socklen_t len = sizeof(addr);
    m_socket_ = accept(m_socket_, (struct sockaddr*)&addr, &len);
    return m_socket_;
}
bool   TcpSocket::Connect(std::string ip, uint16_t port, int timeout)
{
    if (timeout == 0)
    {
        struct sockaddr_in addr;
        addr.sin_family = AF_INET;
        addr.sin_port = htons(port);
        addr.sin_addr.s_addr = inet_addr(ip.c_str());
        if (connect(m_socket_, (struct sockaddr*)&addr, sizeof(addr)) < 0)
        {
            CatLog::__Write_Log("/home/roots/CLionProjects/FFmpegAAc/kxyLog",DEBUG_LOG("<socket=%d> connect failed"));
            return false;
        }
    }
    return false;
}
void   TcpSocket::Close()
{
    close(m_socket_);
}
void   TcpSocket::ShutdownWrite()
{
    shutdown(m_socket_, SHUT_WR);
    m_socket_ = 0;
}