#include "RtspInterleavedTransport.h"

#include <cstring>
#include <limits>
#include <utility>
#include <vector>

namespace media::transport
{

RtspInterleavedTransport::RtspInterleavedTransport(
    uint64_t id,
    std::weak_ptr<TcpConnection> connection,
    uint8_t rtp_channel,
    uint8_t rtcp_channel)
    : MediaTransportBase(id),
      connection_(std::move(connection)),
      rtp_channel_(rtp_channel),
      rtcp_channel_(rtcp_channel)
{
    auto locked = connection_.lock();
    SetState(locked && !locked->IsClosed()
        ? MediaTransportState::Connected
        : MediaTransportState::Failed);
}

MediaTransportProtocol RtspInterleavedTransport::Protocol() const noexcept
{
    return MediaTransportProtocol::RtspInterleaved;
}

SendResult RtspInterleavedTransport::Send(MediaPacketType type,
                                          const uint8_t* data,
                                          size_t size,
                                          bool retransmit)
{
    (void)retransmit;
    if (State() != MediaTransportState::Connected)
    {
        return IsClosed() ? SendResult::Closed : SendResult::NotWritable;
    }
    if (!data || size == 0 || size > std::numeric_limits<uint16_t>::max())
    {
        return SendResult::Failed;
    }

    auto connection = connection_.lock();
    if (!connection || connection->IsClosed())
    {
        SetState(MediaTransportState::Closed);
        return SendResult::Closed;
    }

    const uint8_t channel = type == MediaPacketType::Rtp
        ? rtp_channel_
        : rtcp_channel_;
    const auto payload_size = static_cast<uint16_t>(size);

    auto allocation = common::MemoryPool::Default().TryAllocate(4 + size);
    if (!allocation) return SendResult::NotWritable;
    allocation[0] = '$';
    allocation[1] = channel;
    allocation[2] = static_cast<uint8_t>((payload_size >> 8) & 0xFF);
    allocation[3] = static_cast<uint8_t>(payload_size & 0xFF);
    std::memcpy(allocation.get() + 4, data, size);

    std::shared_ptr<uint8_t[]> storage(std::move(allocation));
    std::shared_ptr<char> bytes(storage, reinterpret_cast<char*>(storage.get()));
    switch (connection->Send(std::move(bytes), static_cast<uint32_t>(4 + size)))
    {
        case TcpConnection::SendResult::Queued: return SendResult::Ok;
        case TcpConnection::SendResult::QueueFull: return SendResult::NotWritable;
        case TcpConnection::SendResult::Closed: return SendResult::Closed;
        default: return SendResult::Failed;
    }
}

void RtspInterleavedTransport::Close()
{
    const auto state = State();
    if (state == MediaTransportState::Closed)
    {
        return;
    }
    SetState(MediaTransportState::Closing);
    DetachPacketSink();
    connection_.reset();
    SetState(MediaTransportState::Closed);
}

MediaPacketIngressResult RtspInterleavedTransport::InputInterleaved(
    uint8_t channel,
    const uint8_t* data,
    size_t size,
    uint64_t receive_time_ms)
{
    MediaPacketType type;
    if (channel == rtp_channel_)
    {
        type = MediaPacketType::Rtp;
    }
    else if (channel == rtcp_channel_)
    {
        type = MediaPacketType::Rtcp;
    }
    else
    {
        return MediaPacketIngressResult::Dropped;
    }

    return PublishPacket(type, data, size, receive_time_ms, channel);
}

} // namespace media::transport
