//
// Created by roots on 2024/9/11.
//

#ifndef FFMPEGAAC_TCPSERVER_H
#define FFMPEGAAC_TCPSERVER_H

#include <memory>
#include <string>
#include <mutex>
#include <chrono>
#include <unordered_map>

#include "Socket.h"
#include "TcpConnection.h"
#include "EventLoop.h"
#include "Acceptor.h"

class TcpServer 
{
public:
    TcpServer(EventLoop* event_loop);
	virtual ~TcpServer();

    virtual bool Start(std::string ip, uint16_t port);
	virtual void Stop();

    std::string GetIPAddress() const
	{ return ip_; }

	uint16_t GetPort() const 
	{ return port_; }
	EventLoop* GetEventLoop() const { return event_loop_; }

protected:
    virtual TcpConnection::Ptr OnConnect(SOCKET sockfd);
	virtual void AddConnection(SOCKET sockfd, TcpConnection::Ptr tcp_conn);
	virtual void RemoveConnection(SOCKET sockfd);

    EventLoop* event_loop_;
	uint16_t port_;
	std::string ip_;
	std::unique_ptr<Acceptor> acceptor_;
	std::atomic<bool> is_started_;
	std::mutex mutex_;
	std::unordered_map<SOCKET, TcpConnection::Ptr> connections_;
};


#endif //FFMPEGAAC_TCPSERVER_H
