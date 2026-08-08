#include "MediaTransportBase.h"

#include <utility>

namespace media::transport
{

MediaTransportBase::MediaTransportBase(uint64_t id) noexcept
    : id_(id)
{
}

uint64_t MediaTransportBase::Id() const noexcept
{
    return id_;
}

MediaTransportState MediaTransportBase::State() const noexcept
{
    return state_.load(std::memory_order_acquire);
}

void MediaTransportBase::SetPacketSink(std::weak_ptr<IMediaPacketSink> sink)
{
    std::lock_guard<std::mutex> lock(sink_mutex_);
    sink_ = std::move(sink);
}

void MediaTransportBase::SetState(MediaTransportState state) noexcept
{
    state_.store(state, std::memory_order_release);
}

void MediaTransportBase::DetachPacketSink()
{
    std::lock_guard<std::mutex> lock(sink_mutex_);
    sink_.reset();
}

MediaPacketIngressResult MediaTransportBase::PublishPacket(MediaPacketType type, const uint8_t* data, size_t size, uint64_t receive_time_ms, int channel)
{
    if (State() != MediaTransportState::Connected || !data || size == 0)
    {
        return MediaPacketIngressResult::Dropped;
    }

    std::shared_ptr<IMediaPacketSink> sink;
    {
        std::lock_guard<std::mutex> lock(sink_mutex_);
        sink = sink_.lock();
    }

    if (!sink)
    {
        return MediaPacketIngressResult::Closed;
    }

    ReceivedMediaPacket packet(type, Id(), receive_time_ms, data, size, channel);
    return sink->OnMediaPacket(std::move(packet));
}

} // namespace media::transport
