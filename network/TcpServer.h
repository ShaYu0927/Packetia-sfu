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
    explicit TcpServer(std::shared_ptr<TaskScheduler> scheduler);
	virtual ~TcpServer();

    virtual bool Start(std::string ip, uint16_t port);
	virtual void Stop();

    std::string GetIPAddress() const
	{ return ip_; }

	uint16_t GetPort() const 
	{ return port_; }
	EventLoop* GetEventLoop() const { return event_loop_; }
    std::shared_ptr<TaskScheduler> GetTaskScheduler() const { return acceptor_->GetTaskScheduler(); }

protected:
    // Construct and install protocol callbacks; the server starts reads after
    // registration. OnConnected runs with disconnect cleanup already installed.
    virtual TcpConnection::Ptr OnConnect(SOCKET sockfd);
    virtual void OnConnected(const TcpConnection::Ptr& conn) {}
	virtual void AddConnection(SOCKET sockfd, TcpConnection::Ptr tcp_conn);
	virtual void RemoveConnection(SOCKET sockfd);

    void ConfigureAcceptor();

    EventLoop* event_loop_ = nullptr;
	uint16_t port_;
	std::string ip_;
	std::unique_ptr<Acceptor> acceptor_;
	std::atomic<bool> is_started_;
	std::mutex mutex_;
	std::unordered_map<SOCKET, TcpConnection::Ptr> connections_;
};


#endif //FFMPEGAAC_TCPSERVER_H
