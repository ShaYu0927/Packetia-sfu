#ifndef _WEBSOCKET_SERVER_H_
#define _WEBSOCKET_SERVER_H_

#include <cstdint>
#include <functional>
#include <memory>
#include <string>

class EventLoop;
class TaskScheduler;

namespace network 
{
namespace websocket 
{

struct WsConnectionInfo 
{
    std::string connId;
    std::string path;
    std::string peerAddr;
};



class WsServer
{
public:
    using OnOpenCallback = std::function<void(const WsConnectionInfo& info)>;
    using OnMessageCallback = std::function<std::string(const std::string& connId,const std::string& message)>;
    using OnCloseCallback = std::function<void(const std::string& connId)>;

public:
    explicit WsServer(EventLoop* event_loop);
    explicit WsServer(std::shared_ptr<TaskScheduler> scheduler);
    ~WsServer();
    WsServer(const WsServer&) = delete;
    WsServer& operator=(const WsServer&) = delete;

    bool Start(const std::string& ip, uint16_t port);
    void Stop();

    bool SendText(const std::string& connId, const std::string& message);
    bool CloseConnection(const std::string& connId);

    void SetOnOpen(OnOpenCallback cb);
    void SetOnMessage(OnMessageCallback cb);
    void SetOnClose(OnCloseCallback cb);

private:
    class Impl;
    std::shared_ptr<Impl> impl_;
};

}
}

#endif /* _WEBSOCKET_SERVER_H_ */
