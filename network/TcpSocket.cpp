//
// Created by roots on 2024/9/10.
//

#include "TcpSocket.h"


TcpSocket::TcpSocket(int m_socket)
    : m_socket_(m_socket)
{
    std::cout << "TcpSocket constructed with socket = " << m_socket_ << std::endl;
}

TcpSocket::~TcpSocket()
{
    std::cout << "TcpSocket destructed with socket = " << m_socket_ << std::endl;
}

int TcpSocket::Create()
{
    m_socket_ = socket(AF_INET, SOCK_STREAM, 0);
    std::cout << "TcpSocket::Create() socket fd = " << m_socket_ << std::endl;
    return m_socket_;
}

bool TcpSocket::Bind(std::string ip, uint16_t port)
{
    struct sockaddr_in addr;
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    addr.sin_addr.s_addr = inet_addr(ip.c_str());

    int ret = bind(m_socket_, (struct sockaddr*)&addr, sizeof(addr));
    std::cout << "TcpSocket::Bind() to " << ip << ":" << port << " ret = " << ret << std::endl;
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
    std::cout << "TcpSocket::Listen() backlog = " << backlog << " ret = " << ret << std::endl;
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
    std::cout << "TcpSocket::Accept() new client fd = " << client_fd << std::endl;
    return client_fd;
}

bool TcpSocket::Connect(std::string ip, uint16_t port, int timeout)
{
    std::cout << "TcpSocket::Connect() to " << ip << ":" << port << " timeout = " << timeout << std::endl;
    if (timeout == 0)
    {
        struct sockaddr_in addr;
        addr.sin_family = AF_INET;
        addr.sin_port = htons(port);
        addr.sin_addr.s_addr = inet_addr(ip.c_str());
        int ret = connect(m_socket_, (struct sockaddr*)&addr, sizeof(addr));
        std::cout << "TcpSocket::Connect() connect ret = " << ret << std::endl;
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
    std::cout << "TcpSocket::Close() socket = " << m_socket_ << std::endl;
    close(m_socket_);
    m_socket_ = -1;
}

void TcpSocket::ShutdownWrite()
{
    std::cout << "TcpSocket::ShutdownWrite() socket = " << m_socket_ << std::endl;
    shutdown(m_socket_, SHUT_WR);
    m_socket_ = 0;
}
