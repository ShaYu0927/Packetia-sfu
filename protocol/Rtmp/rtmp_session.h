#ifndef PACKETIA_PROTOCOL_RTMP_SESSION_H_
#define PACKETIA_PROTOCOL_RTMP_SESSION_H_

#include <memory>
#include <utility>
#include "TcpSession.h"
#include "rtmp_protocol.h"
#include "rtmp_transport.h"

namespace protocol::rtmp
{
class RtmpSession;

class IRtmpMessageHandler
{
public:
    virtual ~IRtmpMessageHandler() = default;
    virtual void OnMessage(RtmpSession& session, const RtmpMessage& message) = 0;
    virtual void OnClosed(RtmpSession& session, int reason) { (void)session; (void)reason; }
};

class RtmpSession : public IRtmpTransportSink,
                    public std::enable_shared_from_this<RtmpSession>
{
public:
    using Ptr = std::shared_ptr<RtmpSession>;

    RtmpSession(TcpConnection::Ptr connection, EndpointRole role);
    RtmpSession(std::shared_ptr<IRtmpTransport> transport, EndpointRole role);

    void Start();
    bool OnRtmpBytes(const uint8_t* data, size_t size) override;
    void OnRtmpTransportClosed(int reason) override;

    void Send(const RtmpMessage& message);
    void SetHandler(const std::shared_ptr<IRtmpMessageHandler>& handler);

private:
    void SendBytes(const std::vector<uint8_t>& bytes);

    std::shared_ptr<IRtmpTransport> transport_;
    EndpointRole role_;
    RtmpProtocol protocol_;
    std::shared_ptr<IRtmpMessageHandler> handler_;
};


}  // namespace protocol::rtmp
#endif
