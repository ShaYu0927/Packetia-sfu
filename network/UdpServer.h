#ifndef _UDPSERVER_H_
#define _UDPSERVER_H_

#include <memory>
#include "UdpSocket.h"
#include "EventLoop.h"

namespace network
{
class IUdpHandler 
{
public:
    using Ptr = std::shared_ptr<IUdpHandler>;
    virtual ~IUdpHandler() = default;

    virtual void OnDatagram(const network::SocketAddr& src,
                            const uint8_t* data,
                            size_t len) = 0;

    virtual void OnClosed(int /*reason*/) {}
    virtual void OnError(int /*err*/) {}
};

class UdpServer 
{
public:
    using Ptr = std::shared_ptr<UdpServer>;

    explicit UdpServer(EventLoop* loop);
    ~UdpServer();

    void SetHandler(IUdpHandler::Ptr h);

    bool Start(const std::string& ip, uint16_t port);
    void Stop();

    bool SendTo(const network::SocketAddr& dst, const uint8_t* data, size_t len);

    int Fd() const { return sock_.Fd(); }

private:
    void OnReadable();

private:
    EventLoop* event_loop_{nullptr};
    std::shared_ptr<TaskScheduler> scheduler_;
    ChannelPtr channel_;

    network::UdpSocket sock_;
    std::atomic<bool> started_{false};

    IUdpHandler::Ptr handler_;
};
}


#endif /* _UDPSERVER_H_ */