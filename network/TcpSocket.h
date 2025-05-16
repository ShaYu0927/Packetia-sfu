//
// Created by roots on 2024/9/10.
//

#ifndef FFMPEGAAC_TCPSOCKET_H
#define FFMPEGAAC_TCPSOCKET_H

#include <cstdint>
#include <string>
#include "Socket.h"
#include "logger.h"

class TcpSocket {
public:
    TcpSocket(int m_socket = -1);
    virtual ~TcpSocket();

    int    Create();
    bool   Bind(std::string ip, uint16_t port);
    bool   Listen(int backlog);
    int    Accept();
    bool   Connect(std::string ip, uint16_t port, int timeout = 0);
    void   Close();
    void   ShutdownWrite();
    int GetSocket() const { return m_socket_; }

private:
    int m_socket_;

};


#endif //FFMPEGAAC_TCPSOCKET_H
