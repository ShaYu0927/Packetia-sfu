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

    auto frame = std::make_shared<std::vector<char>>(4 + size);
    (*frame)[0] = '$';
    (*frame)[1] = static_cast<char>(channel);
    (*frame)[2] = static_cast<char>((payload_size >> 8) & 0xFF);
    (*frame)[3] = static_cast<char>(payload_size & 0xFF);
    std::memcpy(frame->data() + 4, data, size);

    TaskScheduler* scheduler = connection->GetTaskScheduler();
    if (!scheduler)
    {
        return SendResult::Failed;
    }

    std::weak_ptr<TcpConnection> weak_connection = connection;
    return scheduler->Post([weak_connection, frame = std::move(frame)] {
        auto locked = weak_connection.lock();
        if (locked && !locked->IsClosed())
        {
            locked->Send(frame->data(), static_cast<uint32_t>(frame->size()));
        }
    }) ? SendResult::Ok : SendResult::Failed;
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
