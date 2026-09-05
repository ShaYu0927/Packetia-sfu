#ifndef _ACCEPTOR_H_
#define _ACCEPTOR_H_    

#include <functional>
#include "Channel.h"
#include "TcpSocket.h"
#include "EventLoop.h"


typedef std::function<void(int)> NewConnectionCallback;


class Acceptor
{
public:
    Acceptor(EventLoop* eventLoop);
	virtual ~Acceptor();

    void SetNewConnectionCallback(const NewConnectionCallback& cb)
	{ new_connection_callback_ = cb; }


    int  Listen(std::string ip, uint16_t port);
	void Close();
    std::shared_ptr<TaskScheduler> GetTaskScheduler() const { return scheduler_; }
    uint16_t GetPort() const { return port_; }

private:
    void OnAccept();

    EventLoop* event_loop_ = nullptr;
    std::shared_ptr<TaskScheduler> scheduler_;
    uint16_t port_ = 0;
	std::mutex mutex_;
    std::unique_ptr<TcpSocket> tcp_socket_;
    ChannelPtr channel_ptr_;
    NewConnectionCallback new_connection_callback_;
};

#endif
