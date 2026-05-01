#ifndef _WEBSOCKET_SERVER_H_
#define _WEBSOCKET_SERVER_H_

#include <cstdint>
#include <functional>
#include <memory>
#include <string>

#include "hv/hsocket.h"
#include "hv/hloop.h"
#include "hv/Buffer.h"
#include "hv/Channel.h"

#ifndef DEFAULT_CONNECT_TIMEOUT
#define DEFAULT_CONNECT_TIMEOUT DEFAULT_HTTP_CONNECT_TIMEOUT
#endif

#include "server/WebSocketServer.h"

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
    WsServer();
    ~WsServer();

    bool Start(const std::string& ip, uint16_t port, int threadNum = 1);
    void Stop();

    bool SendText(const std::string& connId, const std::string& message);
    bool CloseConnection(const std::string& connId);

    void SetOnOpen(OnOpenCallback cb);
    void SetOnMessage(OnMessageCallback cb);
    void SetOnClose(OnCloseCallback cb);

private:
    class Impl;
    std::unique_ptr<Impl> impl_;
};

}
}

#endif /* _WEBSOCKET_SERVER_H_ */