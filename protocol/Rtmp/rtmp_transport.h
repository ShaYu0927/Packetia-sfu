#ifndef PACKETIA_PROTOCOL_RTMP_TRANSPORT_H_
#define PACKETIA_PROTOCOL_RTMP_TRANSPORT_H_

#include "TcpConnection.h"

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <mutex>

namespace protocol::rtmp
{

enum class RtmpTransportState
{
    Created = 0,
    Connected,
    Closing,
    Closed,
    Failed
};

enum class RtmpTransportSendResult
{
    Ok = 0,
    Failed,
    Closed,
    NotWritable
};

class IRtmpTransportSink
{
public:
    virtual ~IRtmpTransportSink() = default;
    virtual bool OnRtmpBytes(const uint8_t* data, size_t size) = 0;
    virtual void OnRtmpTransportClosed(int reason) = 0;
};

class IRtmpTransport
{
public:
    virtual ~IRtmpTransport() = default;

    virtual RtmpTransportState State() const noexcept = 0;
    virtual bool Start(std::weak_ptr<IRtmpTransportSink> sink) = 0;
    virtual RtmpTransportSendResult Send(const uint8_t* data, size_t size) = 0;
    virtual void Close() = 0;
};

class RtmpTcpTransport final
    : public IRtmpTransport,
      public std::enable_shared_from_this<RtmpTcpTransport>
{
public:
    explicit RtmpTcpTransport(TcpConnection::Ptr connection);
    ~RtmpTcpTransport() override { Close(); }

    RtmpTransportState State() const noexcept override;
    bool Start(std::weak_ptr<IRtmpTransportSink> sink) override;
    RtmpTransportSendResult Send(const uint8_t* data, size_t size) override;
    void Close() override;

private:
    bool HandleRead(BufferReader& buffer);
    void HandleClosed(int reason);

    TcpConnection::Ptr connection_;
    std::atomic<RtmpTransportState> state_{RtmpTransportState::Created};
    std::mutex sink_mutex_;
    std::weak_ptr<IRtmpTransportSink> sink_;
};

} // namespace protocol::rtmp

#endif /* PACKETIA_PROTOCOL_RTMP_TRANSPORT_H_ */
