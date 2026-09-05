#include "rtmp_transport.h"

#include <cstring>
#include <limits>
#include <utility>
#include <vector>

namespace protocol::rtmp
{

RtmpTcpTransport::RtmpTcpTransport(TcpConnection::Ptr connection)
    : connection_(std::move(connection))
{
    if (!connection_ || connection_->IsClosed())
    {
        state_.store(RtmpTransportState::Failed, std::memory_order_release);
    }
}

RtmpTransportState RtmpTcpTransport::State() const noexcept
{
    return state_.load(std::memory_order_acquire);
}

bool RtmpTcpTransport::Start(std::weak_ptr<IRtmpTransportSink> sink)
{
    RtmpTransportState expected = RtmpTransportState::Created;
    if (!state_.compare_exchange_strong(expected,
                                        RtmpTransportState::Connected,
                                        std::memory_order_acq_rel))
    {
        return expected == RtmpTransportState::Connected;
    }
    if (!connection_ || connection_->IsClosed())
    {
        state_.store(RtmpTransportState::Failed, std::memory_order_release);
        return false;
    }
    auto self = weak_from_this();
    if (self.expired())
    {
        state_.store(RtmpTransportState::Failed, std::memory_order_release);
        return false;
    }
    {
        std::lock_guard<std::mutex> lock(sink_mutex_);
        sink_ = std::move(sink);
    }

    std::weak_ptr<RtmpTcpTransport> weak = self;
    connection_->SetReadCallback(
        [weak](TcpConnection::Ptr, BufferReader& buffer) {
            auto transport = weak.lock();
            return transport && transport->HandleRead(buffer);
        });
    connection_->SetCloseCallback(
        [weak](TcpConnection::Ptr) {
            if (auto transport = weak.lock())
            {
                transport->HandleClosed(0);
            }
        });
    connection_->Start();
    return true;
}

RtmpTransportSendResult RtmpTcpTransport::Send(const uint8_t* data,
                                                size_t size)
{
    if (State() != RtmpTransportState::Connected)
    {
        return State() == RtmpTransportState::Closed
            ? RtmpTransportSendResult::Closed
            : RtmpTransportSendResult::NotWritable;
    }
    if (!data || size == 0 || size > std::numeric_limits<uint32_t>::max())
    {
        return RtmpTransportSendResult::Failed;
    }
    auto connection = connection_;
    if (!connection || connection->IsClosed())
    {
        state_.store(RtmpTransportState::Closed, std::memory_order_release);
        return RtmpTransportSendResult::Closed;
    }

    switch (connection->Send(reinterpret_cast<const char*>(data), static_cast<uint32_t>(size)))
    {
        case TcpConnection::SendResult::Queued: return RtmpTransportSendResult::Ok;
        case TcpConnection::SendResult::QueueFull: return RtmpTransportSendResult::NotWritable;
        case TcpConnection::SendResult::Closed: return RtmpTransportSendResult::Closed;
        default: return RtmpTransportSendResult::Failed;
    }
}

void RtmpTcpTransport::Close()
{
    const auto state = State();
    if (state == RtmpTransportState::Closing ||
        state == RtmpTransportState::Closed)
    {
        return;
    }
    state_.store(RtmpTransportState::Closing, std::memory_order_release);
    {
        std::lock_guard<std::mutex> lock(sink_mutex_);
        sink_.reset();
    }
    if (connection_ && !connection_->IsClosed())
    {
        connection_->close();
    }
    state_.store(RtmpTransportState::Closed, std::memory_order_release);
}

bool RtmpTcpTransport::HandleRead(BufferReader& buffer)
{
    const size_t readable = buffer.ReadableBytes();
    if (readable == 0)
    {
        return true;
    }

    std::shared_ptr<IRtmpTransportSink> sink;
    {
        std::lock_guard<std::mutex> lock(sink_mutex_);
        sink = sink_.lock();
    }
    if (!sink)
    {
        return false;
    }
    const bool accepted = sink->OnRtmpBytes(
        reinterpret_cast<const uint8_t*>(buffer.Peek()), readable);
    if (accepted)
    {
        buffer.Retrieve(readable);
    }
    return accepted;
}

void RtmpTcpTransport::HandleClosed(int reason)
{
    if (state_.exchange(RtmpTransportState::Closed,
                        std::memory_order_acq_rel) == RtmpTransportState::Closed)
    {
        return;
    }

    std::shared_ptr<IRtmpTransportSink> sink;
    {
        std::lock_guard<std::mutex> lock(sink_mutex_);
        sink = sink_.lock();
        sink_.reset();
    }
    if (sink)
    {
        sink->OnRtmpTransportClosed(reason);
    }
}

} // namespace protocol::rtmp
