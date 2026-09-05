#ifndef PACKETIA_PROTOCOL_RTMP_SERVER_H_
#define PACKETIA_PROTOCOL_RTMP_SERVER_H_

#include <memory>
#include <unordered_map>

#include "TcpServer.h"
#include "rtmp_session.h"

namespace protocol::rtmp {

class RtmpServer final : public TcpServer
{
public:
    explicit RtmpServer(EventLoop* event_loop);
    ~RtmpServer() override { Stop(); }

    void SetHandler(std::shared_ptr<IRtmpMessageHandler> handler);

protected:
    TcpConnection::Ptr OnConnect(SOCKET sockfd) override;
    void RemoveConnection(SOCKET sockfd) override;

private:
    std::unordered_map<SOCKET, RtmpSession::Ptr> sessions_;
    std::shared_ptr<IRtmpMessageHandler> handler_;
};

}  // namespace protocol::rtmp

#endif
