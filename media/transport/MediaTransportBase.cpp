#include "MediaTransportBase.h"
#include "quality/SendSideController.h"

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
    std::shared_ptr<media::SendSideController> controller;
    {
        std::lock_guard<std::mutex> lock(controller_mutex_);
        controller = send_controller_;
    }
    if (controller)
        controller->SetNetworkAvailable(state == MediaTransportState::Connected);
}

std::shared_ptr<media::SendSideController> MediaTransportBase::GetSendSideController()
{
    std::lock_guard<std::mutex> lock(controller_mutex_);
    if (!send_controller_)
    {
        media::NetworkControllerConfig config;
        config.network_available = State() == MediaTransportState::Connected;
        send_controller_ = std::make_shared<media::SendSideController>(config);
    }
    return send_controller_;
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
    if (type == MediaPacketType::Rtcp)
    {
        std::shared_ptr<media::SendSideController> controller;
        {
            std::lock_guard<std::mutex> lock(controller_mutex_);
            controller = send_controller_;
        }
        if (controller && !controller->OnRtcpPacket(data, size, receive_time_ms))
            return MediaPacketIngressResult::Dropped;
    }
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
