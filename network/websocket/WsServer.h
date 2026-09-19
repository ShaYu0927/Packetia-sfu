#ifndef _WEBSOCKET_SERVER_H_
#define _WEBSOCKET_SERVER_H_

#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include "WsLimits.h"

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

struct WsServerOptions
{
    WsLimits limits;
    uint32_t service_interval_ms = 5;
    std::size_t max_writable_batch = 256;
    std::size_t max_pending_handshakes = 128;
    uint32_t handshake_timeout_seconds = 10;
};

struct WsServerStats
{
    std::size_t active_connections = 0;
    std::size_t queued_bytes = 0;
    std::size_t queued_messages = 0;
    uint64_t accepted_connections = 0;
    uint64_t rejected_connections = 0;
    uint64_t received_messages = 0;
    uint64_t sent_messages = 0;
    uint64_t rejected_messages = 0;
};


class WsServer
{
public:
    using OnOpenCallback = std::function<void(const WsConnectionInfo& info)>;
    using OnMessageCallback = std::function<std::string(const std::string& connId,const std::string& message)>;
    using OnCloseCallback = std::function<void(const std::string& connId)>;

public:
    explicit WsServer(EventLoop* event_loop, WsServerOptions options = {});
    explicit WsServer(std::shared_ptr<TaskScheduler> scheduler, WsServerOptions options = {});
    ~WsServer();
    WsServer(const WsServer&) = delete;
    WsServer& operator=(const WsServer&) = delete;

    bool Start(const std::string& ip, uint16_t port);
    void Stop();

    bool SendText(const std::string& connId, const std::string& message);
    bool CloseConnection(const std::string& connId);
    WsServerStats GetStats() const;

    // Callbacks normally execute on the I/O owner, outside session/registry
    // locks. After the loop exits, Stop runs final close callbacks on its caller.
    // Keep them nonblocking; post expensive work to WorkerService and return
    // an empty string, then SendText from that worker when the reply is ready.
    // SendText is thread-safe and reports bounded queue admission, not delivery.
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
